/*
 * kvspace.c — KVSpace SHM 存储引擎: ART 树 + slotsboxmalloc
 */

#define _GNU_SOURCE
/* header-only 模式：两个依赖库的实现体在本 TU 编译，不再链接它们的 .so。
 * blockmalloc 必须先于 slotsboxmalloc（后者在其实现体里调用前者）。 */
#define BLOCKMALLOC_IMPLEMENTATION
#include <blockmalloc/blockmalloc.h>
#define SBO_MIN_SHARD_BYTES (1ULL << 40)
#define SLOTSBOXMALLOC_IMPLEMENTATION
#include "slotsboxmalloc/slotsboxobj.h"
#include "kvspace_shm.h"
#include <kvspace/kvspace.h>
#include <kvspace/kvlang_xvalue.h>
#include "xvalue_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KVS_MAGIC "kvspace-c.v5"
#define ART_PREFIX_MAX 10
/* ART block = one N4/N16 node; N48/N256 take a run of consecutive blocks.
   The pool only appends (nodes are never freed), so runs are bump-allocated. */
#define ART_BLOCK_SZ 104
#define ART_SLAB_INIT (64UL * 1024 * 1024)
/* blocks_init picks 30-bit ids only if the initial pool holds > 8191 blocks;
   a smaller pool would be capped at 16384 blocks forever. */
#define SBO_HEAD_POOL_INIT (4UL * 1024 * 1024)
/* Reserved VA per region: grow in place, base never moves.
   data grows x64 per step; 2^40 fits 8 * 64^6 = 512GB. */
#define REGION_RESERVE (1ULL << 38)
/* ART ids are 30-bit: reserve <= 2^30 blocks. */
#define ART_RESERVE (1ULL << 36)
#define DATA_RESERVE (1ULL << 40)
/* Values >= 64KB are level-3+ objects (32KB aligned): punch pages on delete. */
#define SBO_PUNCH_MIN (64UL * 1024)

static int reserved_meta_path(const char *key) {
    static const char root[] = "/.kvspace-meta";
    return strncmp(key, root, sizeof root - 1) == 0 &&
           (key[sizeof root - 1] == 0 || key[sizeof root - 1] == '/');
}

enum { ART_N4 = 0,
       ART_N16 = 1,
       ART_N48 = 2,
       ART_N256 = 3,
       ART_MOVED = 255 };

/* v5：{has_value, box_offset} 合并成单个 8B 对齐原子字，禁止裸位域访问。
 * val = bit0 has_value | bit1..63 box_offset（MOVED 节点用 box_offset 存新 id）。 */
typedef struct {
    uint8_t type, prefix[ART_PREFIX_MAX], prefix_len;
    uint16_t count;
    uint16_t _pad;
    uint64_t val;
} art_hdr_t;

#define AV_HAS(v) ((int)((v) & 1u))
#define AV_OFF(v) ((uint64_t)((v) >> 1))
#define AV_MAKE(has, off) ((((uint64_t)(off)) << 1) | ((has) ? 1ull : 0ull))
static inline uint64_t art_val_load(const art_hdr_t *h) {
    return __atomic_load_n(&h->val, __ATOMIC_ACQUIRE);
}
static inline void art_val_store(art_hdr_t *h, uint64_t v) {
    __atomic_store_n(&h->val, v, __ATOMIC_RELEASE);
}
static inline bool art_val_cas(art_hdr_t *h, uint64_t *exp, uint64_t nv) {
    return __atomic_compare_exchange_n(&h->val, exp, nv, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

typedef struct {
    art_hdr_t h;
    uint8_t keys[4];
    int32_t children[4];
} art_n4_t;
typedef struct {
    art_hdr_t h;
    uint8_t keys[16];
    int32_t children[16];
} art_n16_t;
typedef struct {
    art_hdr_t h;
    uint8_t index[256];
    int32_t children[48];
} art_n48_t;
typedef struct {
    art_hdr_t h;
    int32_t children[256];
} art_n256_t;

_Static_assert(sizeof(art_n4_t) <= ART_BLOCK_SZ && sizeof(art_n16_t) <= ART_BLOCK_SZ,
               "N4/N16 must fit one ART block");

static int art_node_sz(int t) {
    return t == ART_N4    ? sizeof(art_n4_t)
           : t == ART_N16 ? sizeof(art_n16_t)
           : t == ART_N48 ? sizeof(art_n48_t)
                          : sizeof(art_n256_t);
}

/* 进程注册表槽：pid + starttime 双重判活（pid 复用），recovery_ack = 已确认的代。 */
#define P2_SLOT_N 64
/* CLAIMING：已 CAS 占位、pid/starttime 尚未写完；扫描者视为存活，避免认领竞态误回收。 */
enum { P2_SLOT_FREE = 0, P2_SLOT_MINE = 1, P2_SLOT_CLAIMING = 3 };
typedef struct {
    uint32_t pid;
    uint32_t state;
    uint64_t starttime;
    uint64_t recovery_ack;
    uint32_t quiesced;   /* Q2 声明（第 3 步接） */
    uint32_t claim_id;   /* open 时领取，1..(1<<30)-1 */
} p2_slot_t;

/* 统一垃圾环：每条 16B，off 恒为真实 box 偏移，状态编码在 meta。
 * meta = state:2 | claim_id:30（EMPTY=0；RESERVED/COMMITTED 带 claim）。 */
#define GARB_CAP (1u << 21)              /* 2,097,152 条 = 32MiB VA */
#define GARB_THRESHOLD (64ull << 20)     /* 触发一轮的垃圾字节 */
#define P2_BARRIER_MS_DEFAULT 200
#define GARB_MAX_CLAIM ((1u << 30) - 1u)
typedef struct {
    uint64_t off;
    uint32_t epoch;
    uint32_t meta;
} garb_ent_t;
enum { GE_EMPTY = 0, GE_RESERVED = 1, GE_COMMITTED = 2 };
#define GE_STATE(m) ((uint32_t)((m) & 3u))
#define GE_CLAIM(m) ((uint32_t)((m) >> 2))
#define GE_MAKE(st, id) ((((uint32_t)(id)) << 2) | (uint32_t)(st))

/* <path>           [kvspace_hdr_t][blocks_meta_t][ART slab ...]
 * <path>.sbo.head  [sbo meta ...]
 * <path>.sbo.data  [sbo data ...] */
typedef struct {
    char magic[12];
    uint64_t art_slab_size, sbo_head_size, sbo_data_size;
    int32_t art_root;
    /* v3: 旁路 gen 表（节点 id 索引，8B/节点）。 */
    uint64_t gens_size;
    uint64_t gen_epoch;        /* 节点分配 epoch（原子递增） */
    uint32_t format_version;   /* = KVSPACE_FORMAT_VERSION */
    uint32_t recovery_epoch;   /* 屏障轮次（u32，模比较） */
    uint32_t ack_seq;          /* 跨进程 futex 字（第 3 步） */
    uint32_t barrier_state;
    uint64_t barrier_pid, barrier_starttime;
    uint64_t garb_cap, garb_head, garb_tail;
    uint64_t committed_bytes;  /* 只由写者改（单写者契约，普通 add） */
    uint64_t freed_bytes;      /* 只由 leader 改 */
    uint64_t garb_freeing;
    uint64_t round_ok, round_aborted, leak_boxes;
    /* v6: Key-set generation. Incremented (release) whenever a key appears (insert/revive)
     * or disappears (delete/tombstone) to invalidate per-handle List caches across processes. */
    uint64_t list_gen;
    uint32_t claim_seq;        /* 30 位发号；耗尽 open 硬失败 */
    p2_slot_t slots[P2_SLOT_N];
} kvspace_hdr_t;

#define ART_OFF (sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t))

/* blocks_init fixes the block-head width (30-bit ids) from the initial size. */
_Static_assert(ART_SLAB_INIT / (ART_BLOCK_SZ + 2) > 32767ULL / 4,
               "ART_SLAB_INIT too small: blockmalloc would pick 14-bit ids");
_Static_assert(ART_RESERVE / (ART_BLOCK_SZ + 8) <= (1ULL << 30),
               "ART_RESERVE too large for 30-bit block ids");
_Static_assert((ART_BLOCK_SZ + 8) % 8 == 0, "ART stride must keep 8B alignment");
_Static_assert(
    SBO_HEAD_POOL_INIT / (sizeof(sbo_box_t) + 2) > 32767ULL / 4,
    "SBO_HEAD_POOL_INIT too small: blockmalloc would pick 14-bit ids");

/* Head layout with root_slots == 1 (checked at open):
   [sbo_meta_t][blocks_meta_t][sbo_lock_t][pool]. Public structs only. */
#define SBO_HEAD_FIXED                                                         \
    (sizeof(sbo_meta_t) + sizeof(blocks_meta_t) + sizeof(sbo_lock_t))
static blocks_meta_t *sbo_pool(sbo_meta_t *m) {
    return sbo_pool_meta(m, 0);
}
static sbo_lock_t *sbo_plock(sbo_meta_t *m) {
    return sbo_pool_lock(m, 0);
}
static uint8_t *sbo_pmem(sbo_meta_t *m) {
    return sbo_pool_mem(m, 0);
}
static sbo_box_t *sbo_box(sbo_meta_t *m, int32_t id) {
    return (sbo_box_t *)(sbo_pmem(m) + (size_t)id * m->block_stride +
                         m->box_off);
}

typedef struct {
    int fd;
    uint8_t *base;
    size_t mapped;
    size_t reserve;
} shm_region_t;

struct kvspace {
    shm_region_t r_art, r_head, r_data, r_gen, r_garb;
    garb_ent_t *garb;             /* .garb 映射（GARB_CAP 条） */
    uint64_t gen_n;               /* gens_at 容量：本进程已映射的节点数 */
    uint64_t art_total_seen;      /* 上次同步时的 ART slab total_size */
    kvspace_hdr_t *hdr;
    uint64_t store_id;   /* 进程内句柄身份（P2 token 用） */
    int p2_slot;         /* 注册表槽；-1 = 未注册 */
    uint32_t pid;        /* open 时的 pid（冷路径判 fork） */
    uint32_t fork_gen;   /* fork 世代：子进程里全局 +1 → 热路径守卫 */
    uint32_t quiesced;   /* Q2 进程内镜像；claim 时 = 1 */
    pthread_t maint_thread;
    uint32_t maint_seq;  /* 进程内私有 futex 字 */
    uint32_t maint_started, maint_stop;  /* started: 0 未建, 1 运行, 2 创建失败 */
    uint32_t reclaim_unavailable;        /* 线程创建失败：环 3/4 满后写硬失败 */
    blocks_meta_t *art_meta;
    uint8_t *art_data;
    sbo_meta_t *sbo_meta;
    uint8_t *sbo_data;
};
/* Bump after key-set changes (insert/revive/delete) to invalidate List caches across processes.
 * Ptr/@ext value changes do not bump this since list_cached re-checks target and mount each call. */
static inline void list_gen_bump(kvspace_t *kv) {
    __atomic_add_fetch(&kv->hdr->list_gen, 1ull, __ATOMIC_RELEASE);
}

/* ---- shm region ---- */
static int region_map(shm_region_t *r, size_t size) {
    if (size > r->reserve)
        return -1;
    if (size <= r->mapped)
        return 0;
    if (mmap(r->base, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
             r->fd, 0) == MAP_FAILED)
        return -1;
    r->mapped = size;
    return 0;
}

/* create: size the file; open: check it. Then reserve VA and map. */
static int region_attach(shm_region_t *r, bool create, size_t size,
                         size_t reserve) {
    struct stat st;
    if (create ? ftruncate(r->fd, (off_t)size) != 0
               : fstat(r->fd, &st) != 0 || st.st_size < (off_t)size)
        return -1;
    r->reserve = size > reserve ? size : reserve;
    void *p = mmap(NULL, r->reserve, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    r->base = p;
    return region_map(r, size);
}

static void region_close(shm_region_t *r) {
    if (r->base)
        munmap(r->base, r->reserve);
    if (r->fd >= 0)
        close(r->fd);
}

/* ── P2: 旁路 gen 表（node id → {struct_gen, value_gen}）──────────── */
static void kv_sbo_free(kvspace_t *kv, uint64_t off);
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id);
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b);
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl);
static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard);
static int32_t art_follow(kvspace_t *kv, int32_t id);

/* 每节点 8B：{struct_gen, value_gen}（v2.2 删 pin 后不再有 refcount 位）。 */
typedef struct {
    uint32_t struct_gen;
    uint32_t value_gen;
} kv_gens_t;

static uint64_t gen_cap(const kvspace_t *kv) {
    blocks_meta_t *m = kv->art_meta;
    uint64_t head = (uint64_t)m->sizeof_block_head;
    return m->total_size / (head + m->block_size);
}

static int32_t art_id_of(kvspace_t *kv, const void *p) {
    if (!p || !kv->art_data)
        return -1;
    return (int32_t)blockid_bydataoffset(kv->art_meta,
                                         (uint64_t)((const uint8_t *)p - kv->art_data));
}

static kv_gens_t *gens_at(kvspace_t *kv, int32_t id) {
    if (id < 0 || !kv->r_gen.base)
        return NULL;
    if ((uint64_t)id >= kv->gen_n)   /* gen_n 由 gens_grow / kv_sync 维护 */
        return NULL;
    return (kv_gens_t *)kv->r_gen.base + id;
}

static void gen_bump_struct(kvspace_t *kv, int32_t id) {
    kv_gens_t *g = gens_at(kv, id);
    if (g)
        __atomic_add_fetch(&g->struct_gen, 1u, __ATOMIC_ACQ_REL);
}

/* value_gen 静止态恒为偶数：就地写前后各 +1（奇=写中），整体替换/删除 +2。 */
static void gen_bump_value(kvspace_t *kv, int32_t id) {
    kv_gens_t *g = gens_at(kv, id);
    if (g)
        __atomic_add_fetch(&g->value_gen, 2u, __ATOMIC_ACQ_REL);
}

static int64_t gen_alloc_epoch(kvspace_t *kv) {
    return (int64_t)__atomic_add_fetch(&kv->hdr->gen_epoch, 1ull, __ATOMIC_ACQ_REL);
}

static int p2_maint_start(kvspace_t *kv);   /* 懒启动（阈值触发） */
static bool p2_claim_alive(kvspace_t *kv, uint32_t claim);
static int kv_sync(kvspace_t *kv);

/* 阈值：生产固定 64MiB；测试构建可用 P2_TEST_GARB_THRESHOLD_MB 调低。 */
static uint64_t p2_garb_threshold(void) {
#ifdef P2_TEST_HOOKS
    const char *v = getenv("P2_TEST_GARB_THRESHOLD_MB");
    if (v) {
        unsigned long mb = strtoul(v, NULL, 10);
        if (mb)
            return (uint64_t)mb << 20;
    }
#endif
    return GARB_THRESHOLD;
}

/* ── 统一垃圾环：第 2 步只记不回收（drain 在第 3 步接）─────────────
 * reserve 必须发生在任何键值/节点变更之前；失败 = 写路径硬错误，无半写。
 * 每个 reserve 之后必须且只能走 commit 或 cancel。 */
static void garb_full_report(kvspace_t *kv) {
    static bool once;
    if (once)
        return;
    once = true;
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    uint32_t m = __atomic_load_n(&kv->garb[h & (GARB_CAP - 1)].meta,
                                 __ATOMIC_ACQUIRE);
    fprintf(stderr,
            "kvspace: garbage ring full (cap=%llu, head=%llu, tail=%llu, "
            "bytes=%llu): head entry #%llu state=%u claim=%u is holding "
            "the drain (no reclaim round yet)\n",
            (unsigned long long)kv->hdr->garb_cap, (unsigned long long)h,
            (unsigned long long)t,
            (unsigned long long)(kv->hdr->committed_bytes -
                                 kv->hdr->freed_bytes),
            (unsigned long long)h, GE_STATE(m), GE_CLAIM(m));
}

static int garb_reserve(kvspace_t *kv, uint64_t *idx) {
    uint32_t claim = kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
    if (!claim)
        return -1;
    uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
    if (t - h >= kv->hdr->garb_cap) {
        garb_full_report(kv);
        return -1;
    }
    if (kv->reclaim_unavailable &&
        kv->hdr->garb_cap - (t - h) < kv->hdr->garb_cap / 4) {
        garb_full_report(kv);        /* 线程建不起来：1/4 安全线硬失败 */
        return -1;
    }
    /* 单写者契约（kvspace-c #58）：entry/tail 只有写者写，普通 release store。
     * leader 只读；唯一的写者进程若死在 store 与 tail 之间，槽位停在 RESERVED
     * 且未发布（i == tail），下一个写者在这里回收死 claim（冷路径）。
     * 活 claim 占着 tail 槽 = 契约被违反 → 硬错误，不猜。 */
    garb_ent_t *e = &kv->garb[t & (GARB_CAP - 1)];
    uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
    if (m != GE_EMPTY) {
        if (GE_STATE(m) != GE_RESERVED || p2_claim_alive(kv, GE_CLAIM(m)))
            return -1;
        __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&e->meta, GE_MAKE(GE_RESERVED, claim), __ATOMIC_RELEASE);
    __atomic_store_n(&kv->hdr->garb_tail, t + 1, __ATOMIC_RELEASE);
    *idx = t;
    return 0;
}

/* bytes 由退役点在“不拿分配器锁”的前提下给出（ViewDel 用 token 缓存的 cap，
 * realloc 用已算出的 cap）；sbo_allocated_size 会取 sbo plock，不能进热路径。 */
static void garb_commit(kvspace_t *kv, uint64_t idx, uint64_t off,
                        uint64_t bytes_retired) {
    garb_ent_t *e = &kv->garb[idx & (GARB_CAP - 1)];
    uint32_t claim = GE_CLAIM(__atomic_load_n(&e->meta, __ATOMIC_ACQUIRE));
    e->off = off;
    e->epoch = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&e->meta, GE_MAKE(GE_COMMITTED, claim), __ATOMIC_RELEASE);
    uint64_t bytes = kv->hdr->committed_bytes + bytes_retired;
    __atomic_store_n(&kv->hdr->committed_bytes, bytes, __ATOMIC_RELEASE);
    if (bytes - __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) >=
        p2_garb_threshold())
        (void)p2_maint_start(kv);    /* 失败已置 reclaim_unavailable */
}

static void garb_cancel(kvspace_t *kv, uint64_t idx) {
    __atomic_store_n(&kv->garb[idx & (GARB_CAP - 1)].meta, GE_EMPTY,
                     __ATOMIC_RELEASE);
}

/* 精确 key 的节点 id（即使 has_value==0 也返回；用于墓碑复活时清 DEAD）。 */
static int32_t p2_node_at(kvspace_t *kv, const char *key) {
    int klen = (int)strlen(key);
    int d = 0;
    int32_t nid = kv->hdr->art_root;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len,
                               (const uint8_t *)key + d, klen - d);
            if (s != h->prefix_len && d + s != klen)
                return -1;
            d += s;
        }
        /* 键可能正好结束在 prefix_len==0 的分支节点上（如 k1 落在根 N4 的子叶），
         * 此时 d==klen，必须在此返回；否则会越界读 key[klen]。 */
        if (d == klen)
            return nid;
        nid = art_child(kv, h, (uint8_t)key[d]);
        d++;
    }
    return -1;
}

/* ── P2: 进程注册表 + 恢复屏障 ────────────────────────────────── */
/* /proc/<pid>/stat 第 22 字段 starttime（pid 复用判据）。macOS 无 /proc。
 * 返回 0=读到；1=进程不存在（ENOENT，判死）；2=不可判定（保守视为存活）。 */
static int p2_proc_starttime(uint32_t pid, uint64_t *out) {
#if defined(__linux__)
    char p[64];
    if (snprintf(p, sizeof p, "/proc/%u/stat", pid) >= (int)sizeof p)
        return 2;
    FILE *f = fopen(p, "r");
    if (!f)
        return errno == ENOENT ? 1 : 2;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    if (!n)
        return 2;
    buf[n] = 0;
    char *rp = strrchr(buf, ')');   /* comm 可含空格，字段从最后一个 ')' 起数 */
    if (!rp)
        return 2;
    char *s = rp + 1;
    int field = 3;                  /* ')' 之后第一个字段是 stat 第 3 字段 */
    while (*s) {
        while (*s == ' ')
            s++;
        if (!*s)
            break;
        if (field == 3 && (*s == 'Z' || *s == 'X'))
            return 1;                   /* 僵尸/已退出：判死（否则会永久占位） */
        if (field == 22) {
            *out = strtoull(s, NULL, 10);
            return 0;
        }
        while (*s && *s != ' ')
            s++;
        field++;
    }
    return 2;
#else
    (void)pid;
    (void)out;
    return 2;                       /* 平台无 liveness：永不判死 */
#endif
}

static bool p2_slot_alive(const p2_slot_t *s) {
    uint64_t st = 0;
    uint32_t state = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
    if (state == P2_SLOT_FREE)
        return false;
    if (state == P2_SLOT_CLAIMING)
        return true;
    int rc = p2_proc_starttime(s->pid, &st);
    if (rc == 1)
        return false;               /* ENOENT：进程已死 */
    if (rc != 0)
        return true;                /* 不可判定 → 保守视为存活 */
    return st == s->starttime;
}

/* open：先回收死槽，再 CAS 认领；仍满才失败（反复崩溃不耗光槽）。 */
static int p2_reg_claim(kvspace_t *kv) {
    uint64_t st = 0;
    if (p2_proc_starttime((uint32_t)getpid(), &st) != 0)
        st = 0;
    uint32_t me = (uint32_t)getpid();
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (v == P2_SLOT_CLAIMING && s->pid == 0) {
            /* 认领者死在 CAS 与写 pid 之间：pid 仍为 0，可回收 */
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            continue;
        }
        if (v != P2_SLOT_FREE && !p2_slot_alive(s))
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t expect = P2_SLOT_FREE;
        if (!__atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_CLAIMING, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        uint32_t cid = __atomic_add_fetch(&kv->hdr->claim_seq, 1u,
                                          __ATOMIC_ACQ_REL);
        if (cid > GARB_MAX_CLAIM) {          /* 不静默回绕 */
            __atomic_store_n(&s->state, P2_SLOT_FREE, __ATOMIC_RELEASE);
            return -3;
        }
        s->pid = me;
        s->starttime = st;
        s->quiesced = 1;   /* 新进程还没有 token：天然静默；runtime 用 view 前显式 Quiesce(0) */
        s->claim_id = cid;
        __atomic_store_n(&s->recovery_ack,
                         __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE),
                         __ATOMIC_RELEASE);
        __atomic_store_n(&s->state, P2_SLOT_MINE, __ATOMIC_RELEASE);
        return i;
    }
    return -2;                      /* 占满：调用方拒绝 open，不静默扩容 */
}

static void p2_reg_release(kvspace_t *kv) {
    if (kv->p2_slot < 0)
        return;
    if (kv->pid != (uint32_t)getpid()) {   /* fork 子进程：句柄无效，不动父进程的槽 */
        kv->p2_slot = -1;
        return;
    }
    p2_slot_t *s = &kv->hdr->slots[kv->p2_slot];
    uint32_t expect = P2_SLOT_MINE;
    __atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_FREE, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    kv->p2_slot = -1;
}

/* 每次 kvspaceView* 入口：屏障代变化 → 记本地代并 ack（token 由 _pad 校验失效）。 */
static void p2_ack_slot(kvspace_t *kv, uint32_t ep) {
    if (kv->p2_slot < 0)
        return;
    __atomic_store_n(&kv->hdr->slots[kv->p2_slot].recovery_ack, ep,
                     __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->hdr->ack_seq, 1u, __ATOMIC_RELEASE);
#if defined(__linux__)
    /* 没有 leader 在等就不要进内核：热路径每 64 条指令一次 ack，
     * 无条件 FUTEX_WAKE 是每次写系统调用。漏唤醒最坏退化为 5ms 分片。 */
    if (__atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE))
        syscall(SYS_futex, &kv->hdr->ack_seq, FUTEX_WAKE, INT_MAX, NULL, NULL,
                0);
#endif
}

/* 表随 ART slab 增长（容量按当前 block 配置重算）。 */
#define GEN_RESERVE (8ULL << 30)    /* 8GiB VA: one gens entry per ART block id */
static int gens_grow(kvspace_t *kv) {
    uint64_t want = gen_cap(kv) * sizeof(kv_gens_t);
    if (want > GEN_RESERVE)
        return -1;
    kv->gen_n = want / sizeof(kv_gens_t);
    kv->art_total_seen = kv->art_meta->total_size;
    if (want <= kv->r_gen.mapped)
        return 0;
    if (!kv->r_gen.base) {
        void *p = mmap(NULL, GEN_RESERVE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p == MAP_FAILED)
            return -1;
        kv->r_gen.base = p;
        kv->r_gen.reserve = GEN_RESERVE;
    }
    if (ftruncate(kv->r_gen.fd, (off_t)want) != 0)
        return -1;
    if (region_map(&kv->r_gen, (size_t)want) != 0)
        return -1;
    kv->hdr->gens_size = want;
    return 0;
}

/* ── P2: 版本化 view token ─────────────────────────────────────── */
/* fork 检测不能每 op 调 getpid()（本机 glibc 不缓存，一次 ~150ns）。
 * 用 pthread_atfork 的子进程钩子把全局世代 +1；热路径只比两个 u32。 */
static uint32_t g_fork_gen;
static pthread_once_t g_atfork_once = PTHREAD_ONCE_INIT;
static void p2_atfork_child(void) {
    __atomic_add_fetch(&g_fork_gen, 1u, __ATOMIC_ACQ_REL);
}
static void p2_atfork_install(void) {
    (void)pthread_atfork(NULL, NULL, p2_atfork_child);
}

#define P2_FORK_GUARD(kv)                                                     \
    ((kv)->fork_gen != __atomic_load_n(&g_fork_gen, __ATOMIC_ACQUIRE))
#define P2_VIEW_GUARD(kv) ((kv)->quiesced || P2_FORK_GUARD(kv))

/* 固定头直读辅助：校验 value 是合法 16 字节头 + 总长自洽；产出头。
   padding（10-15 全 6 字节）非零或总长溢出即失败——旧 8 字节 wire 的 body 首字节会
   落进 padding 或被当成超长 attrslen，因此在这里就被挡住，不做兼容。 */
static int head_of(const uint8_t *val, int32_t val_len, kvspaceXvalueHead *out) {
    if (!val || val_len < (int32_t)KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    memcpy(out, val, sizeof *out);
    /* padding 与总长由 `TotalLen` 一处判掉（它同时算溢出与 padding 为零）。
       head 里已没有 `reserved` 与 `layout` 两个字段，旧模型那两句
       `storetype > ST_VARLEN || ptr > PTR_EXT` 的对应字段也不存在。 */
    const uint64_t total = kvspaceXvalueHeadTotalLen(out);
    if (total == 0 || total > (uint64_t)val_len)
        return -1;
    /* attr 区在这里整体过一遍：tag 2（旧 reallen）出现即拒绝——那是已删除的
       格式，把它误读成有效长度会让跨版本读到错数据。head 没有 layout/reserved
       位可判，bodylen 就是有效长度。历史 wire 的 attr 尾部残余字节不算畸形——
       扫描器与 Rust 同语义，忽略不足一条记录的尾巴。 */
    {
        const uint8_t *langtype = NULL;
        uint16_t langtype_len = 0;
        if (kvspace_c_scan_attrs(val, total, &langtype, &langtype_len) != 0)
            return -1;
    }
    return 0;
}

/* Write validation: head_of 已校验总长，这里只剩容量判定。
 * 总长只由 head 一处算出（16 + bodylen + attrslen），attr 区没有前缀。 */
static int value_ok(const uint8_t *val, int32_t val_len,
                    kvspaceXvalueHead *out) {
    if (head_of(val, val_len, out) != 0)
        return -1;
    return 0;
}

/* 前缀解析：path 的最后一个字节落在哪个节点；depth = 该节点压缩前缀被 path 覆盖的字节数
 * （可能 < prefix_len，即边界落在前缀内部）。用于 short 键（整键就是一个节点的前缀）。 */
/* 前缀形态：parent_ref 的 block_id/depth 指向目录节点；只走 rel。 */
/* Hot-path write: in-place same-length body write. Returns 0 on success, 1 to fall back.
 * The attr region immediately follows body (at 8+len), so only writes matching the current
 * header len are accepted; variable-length writes fall back to full-value write. */
/* 热路径删：tombstone + value_gen++（box 释放走延迟队列）。 */
/* ── 第 3 步：屏障轮 / 维护线程 / Q1-Q2 ─────────────────────────── */

static long long p2_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int p2_barrier_ms(void) {
    const char *v = getenv("P2_BARRIER_MS");
    int ms = v ? atoi(v) : P2_BARRIER_MS_DEFAULT;
    return ms < 1 ? 1 : (ms > 5000 ? 5000 : ms);
}

#if defined(__linux__)
static void p2_futex_wait(uint32_t *addr, uint32_t val, int ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT, val, &ts, NULL, 0);
}
static void p2_futex_wake(uint32_t *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}
static void p2_futex_wait_priv(uint32_t *addr, uint32_t val, int ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
    syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, val, &ts, NULL, 0);
}
static void p2_futex_wake_priv(uint32_t *addr) {
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, INT_MAX, NULL, NULL, 0);
}
#else
static void p2_futex_wait(uint32_t *a, uint32_t v, int ms) { (void)a; (void)v; usleep((useconds_t)ms * 1000); }
static void p2_futex_wake(uint32_t *a) { (void)a; }
static void p2_futex_wait_priv(uint32_t *a, uint32_t v, int ms) { (void)a; (void)v; usleep((useconds_t)ms * 1000); }
static void p2_futex_wake_priv(uint32_t *a) { (void)a; }
#endif

/* 选举字 = state:2 | claim_id:30（与环条目同一编码），单词 CAS，无 CLAIMING 过渡态。 */
enum { P2_BARRIER_FREE = 0, P2_BARRIER_OWNED = 1 };

static uint32_t p2_my_claim(kvspace_t *kv) {
    return kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
}

static bool p2_claim_alive(kvspace_t *kv, uint32_t claim) {
    if (!claim)
        return false;
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (__atomic_load_n(&s->claim_id, __ATOMIC_ACQUIRE) != claim)
            continue;
        return p2_slot_alive(s);
    }
    return false;
}

static bool p2_elect(kvspace_t *kv) {
    uint32_t mine = p2_my_claim(kv);
    if (!mine)
        return false;
    uint32_t own = GE_MAKE(P2_BARRIER_OWNED, mine);
    for (;;) {
        uint32_t cur = __atomic_load_n(&kv->hdr->barrier_state, __ATOMIC_ACQUIRE);
        if (cur == GE_MAKE(P2_BARRIER_FREE, 0)) {
            if (__atomic_compare_exchange_n(&kv->hdr->barrier_state, &cur, own,
                                            false, __ATOMIC_ACQ_REL,
                                            __ATOMIC_ACQUIRE))
                return true;
            continue;
        }
        if (GE_STATE(cur) != P2_BARRIER_OWNED)
            return false;                 /* 非法状态：不猜 */
        if (GE_CLAIM(cur) == mine)
            return false;                 /* 本进程已有 leader 线程在跑 */
        if (p2_claim_alive(kv, GE_CLAIM(cur)))
            return false;                 /* 活着的 owner：跳过，不等待 */
        if (__atomic_compare_exchange_n(&kv->hdr->barrier_state, &cur, own, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return true;                  /* 死者接管 */
    }
}

static void p2_release_busy(kvspace_t *kv) {
    uint32_t mine = GE_MAKE(P2_BARRIER_OWNED, p2_my_claim(kv));
    __atomic_compare_exchange_n(&kv->hdr->barrier_state, &mine,
                                GE_MAKE(P2_BARRIER_FREE, 0), false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}

/* 每槽满足：state != MINE || recovery_ack == r || quiesced。
 * 只在未满足且未判死的槽上读 /proc；判死结果本轮缓存。 */
static bool p2_wait_acks(kvspace_t *kv, uint32_t r, int deadline_ms) {
    uint8_t dead[P2_SLOT_N] = {0};
    long long t0 = p2_now_ms();
    for (;;) {
        uint32_t seq = __atomic_load_n(&kv->hdr->ack_seq, __ATOMIC_ACQUIRE);
        bool missing = false;
        int first = -1;
        for (int i = 0; i < P2_SLOT_N; i++) {
            p2_slot_t *s = &kv->hdr->slots[i];
            if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
                continue;
            if (__atomic_load_n(&s->recovery_ack, __ATOMIC_ACQUIRE) == r)
                continue;
            if (__atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE))
                continue;
            if (!dead[i]) {
                uint64_t ts = 0;
                int rc = p2_proc_starttime(s->pid, &ts);
                if (rc == 1 || (rc == 0 && ts != s->starttime))
                    dead[i] = 1;
            }
            if (dead[i])
                continue;
            missing = true;
            if (first < 0)
                first = i;
        }
        if (!missing)
            return true;
        long long left = (long long)deadline_ms - (p2_now_ms() - t0);
        if (left <= 0 || __atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE)) {
            if (first >= 0) {
                p2_slot_t *s = &kv->hdr->slots[first];
                fprintf(stderr,
                        "kvspace: barrier round %u abandoned (waited %dms): "
                        "slot %d pid=%u start=%llu ack=%llu quiesced=%u\n",
                        r, deadline_ms, first, s->pid,
                        (unsigned long long)s->starttime,
                        (unsigned long long)__atomic_load_n(&s->recovery_ack,
                                                            __ATOMIC_ACQUIRE),
                        __atomic_load_n(&s->quiesced, __ATOMIC_ACQUIRE));
            }
            return false;
        }
        p2_futex_wait(&kv->hdr->ack_seq, seq, (int)(left < 5 ? left : 5));
    }
}

/* 严格 FIFO：EMPTY 推进；RESERVED 活 claim → 停，死 claim → 取消+审计；
 * COMMITTED 且 epoch < r 才释放（cap 在 free 之前取）。 */
static int p2_drain(kvspace_t *kv, uint32_t r) {
    int freed = 0;
    uint64_t i = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
    for (;;) {
        /* 环尾之外是未写条目：到 tail 必须停，否则会把 head 推过 tail
         * （tail-head 下溢后 reserve 永久失败）。 */
        if (i >= __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE))
            goto out;
        garb_ent_t *e = &kv->garb[i & (GARB_CAP - 1)];
        uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
        uint32_t st = GE_STATE(m);
        if (st == GE_EMPTY) {
            __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (st == GE_RESERVED) {
            if (p2_claim_alive(kv, GE_CLAIM(m)))
                goto out;
            __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
            __atomic_add_fetch(&kv->hdr->leak_boxes, 1, __ATOMIC_ACQ_REL);
            fprintf(stderr,
                    "kvspace: garbage entry %llu reserved by dead claim %u: "
                    "leaked\n", (unsigned long long)i, GE_CLAIM(m));
            __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
            i++;
            continue;
        }
        if (st != GE_COMMITTED)
            goto out;                 /* 非法状态：停，不猜 */
        if (!((int32_t)(r - __atomic_load_n(&e->epoch, __ATOMIC_ACQUIRE)) > 0))
            goto out;                 /* 未到轮次 */
        uint64_t off = __atomic_load_n(&e->off, __ATOMIC_ACQUIRE);
        uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);  /* free 之前取 */
        __atomic_store_n(&kv->hdr->garb_freeing, off, __ATOMIC_RELEASE);
        __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
        __atomic_store_n(&kv->hdr->garb_head, i + 1, __ATOMIC_RELEASE);
#ifdef P2_TEST_HOOKS
        {   /* 崩溃窗口：head 已推进、box 未 free —— 只漏不双 free */
            const char *v = getenv("P2_TEST_HOLD_DRAIN_MS");
            if (v && atoi(v) > 0)
                usleep((useconds_t)atoi(v) * 1000);
        }
#endif
        kv_sbo_free(kv, off);
        __atomic_store_n(&kv->hdr->freed_bytes,
                         kv->hdr->freed_bytes + cap, __ATOMIC_RELEASE);
        i++;
        freed++;
    }
out:
    __atomic_store_n(&kv->hdr->garb_freeing, 0, __ATOMIC_RELEASE);
    return freed;
}

/* 一轮：选举 → 清崩溃标记 → 推进 epoch → 等 ack → drain → 回收死槽。
 * 返回释放条数；-1 = 别人持有；-2 = 超时放弃（零 free）。 */
static int p2_round(kvspace_t *kv) {
#if !defined(__linux__)
    (void)kv;
    return 0;                             /* 无 /proc：不推进、不回收 */
#else
    if (!kv || kv->hdr->art_root < 0)
        return 0;
    if (kv->pid != (uint32_t)getpid())
        return -1;                        /* fork 子进程：句柄无效 */
    if (!p2_elect(kv))
        return -1;
    /* 别的进程可能扩过 ART/SBO/gen：drain 前必须把本进程映射同步到当前尺寸，
     * 否则 sbo_allocated_size / kv_sbo_free 会越界访问。 */
    if (kv_sync(kv) != 0) {
        p2_release_busy(kv);
        return -1;
    }
    uint64_t pending = __atomic_exchange_n(&kv->hdr->garb_freeing, 0,
                                           __ATOMIC_ACQ_REL);
    if (pending) {
        __atomic_add_fetch(&kv->hdr->leak_boxes, 1, __ATOMIC_ACQ_REL);
        fprintf(stderr, "kvspace: previous leader died freeing box %llu: "
                        "leaked\n", (unsigned long long)pending);
    }
    uint32_t r = __atomic_add_fetch(&kv->hdr->recovery_epoch, 1u,
                                    __ATOMIC_ACQ_REL);
    if (!p2_wait_acks(kv, r, p2_barrier_ms())) {
        __atomic_add_fetch(&kv->hdr->round_aborted, 1, __ATOMIC_ACQ_REL);
        p2_release_busy(kv);
        return -2;
    }
    int freed = p2_drain(kv, r);
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = P2_SLOT_MINE;
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (p2_slot_alive(s))
            continue;
        __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    __atomic_add_fetch(&kv->hdr->round_ok, 1, __ATOMIC_ACQ_REL);
    p2_release_busy(kv);
    return freed;
#endif
}

static void *p2_maint(void *arg) {
    kvspace_t *kv = arg;
    while (!__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE)) {
        uint32_t seq = __atomic_load_n(&kv->maint_seq, __ATOMIC_ACQUIRE);
        if (__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE))
            break;
        p2_futex_wait_priv(&kv->maint_seq, seq, 50);
        if (__atomic_load_n(&kv->maint_stop, __ATOMIC_ACQUIRE))
            break;
        if (kv->hdr->committed_bytes -
                __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) <
            p2_garb_threshold())
            continue;
        p2_round(kv);                 /* 内含选举；忙/超时都直接返回 */
    }
    return NULL;
}

/* 懒启动：open 不建线程；未回收垃圾越过 64MiB 时才建（open / commit 后检查）。
 * 每个进程最多一个；创建失败置 reclaim_unavailable 并由写路径在环 3/4 满时硬失败。 */
static int p2_maint_start(kvspace_t *kv) {
#if !defined(__linux__)
    (void)kv;
    return 0;                        /* macOS：不回收，无线程 */
#else
    uint32_t st = 0;
    if (!__atomic_compare_exchange_n(&kv->maint_started, &st, 1u, false,
                                     __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
        return st == 2u ? -1 : 0;    /* 已启动 / 已知失败 */
    if (kv->pid != (uint32_t)getpid()) {
        __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
        return 0;                    /* fork 子进程不建 */
    }
    if (pthread_create(&kv->maint_thread, NULL, p2_maint, kv) != 0) {
        __atomic_store_n(&kv->maint_started, 2u, __ATOMIC_RELEASE);
        kv->reclaim_unavailable = 1;
        fprintf(stderr, "kvspace: maintenance thread create failed; reclaim "
                        "unavailable (writes hard-fail when the ring is 3/4 "
                        "full)\n");
        return -1;
    }
    return 0;
#endif
}

static void p2_maint_stop(kvspace_t *kv) {
    if (kv->pid != (uint32_t)getpid()) {   /* fork 子进程里没有该线程 */
        __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
        return;
    }
    if (__atomic_load_n(&kv->maint_started, __ATOMIC_ACQUIRE) != 1u)
        return;                            /* 未启动 / 创建失败 */
    __atomic_store_n(&kv->maint_stop, 1, __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->maint_seq, 1u, __ATOMIC_RELEASE);
    p2_futex_wake_priv(&kv->maint_seq);
    pthread_join(kv->maint_thread, NULL);
    __atomic_store_n(&kv->maint_started, 0u, __ATOMIC_RELEASE);
}

/* Q1：op 边界 ack。0 = 已 ack；1 = epoch 变了（先作废 token 再来）；-1 = 非法。 */
/* Q2：显式静默声明。on 只接受 0/1；幂等；状态变化唤醒 leader。
 * 前置：调用线程已作废本进程全部 view token。声明期间 View* 一律 -1。 */
#ifdef P2_TEST_HOOKS
/* 对齐验收：块头 8、stride 8 倍数，且每个块的 val（偏移 16）不跨 64B 行。
 * 返回 -1 = 通过；>=0 = 第一个坏块 id；-2 = 块头宽度不是 8。 */
int64_t kvspaceShmArtAlignCheck(kvspace_t *kv) {
    if (!kv || !kv->art_meta)
        return -2;
    blocks_meta_t *m = kv->art_meta;
    if (m->sizeof_block_head != 8)
        return -2;
    uint64_t stride = (uint64_t)m->sizeof_block_head + m->block_size;
    if (stride % 8)
        return -2;
    uint64_t n = m->total_size / stride;
    for (uint64_t id = 0; id < n; id++) {
        uint64_t off = blockdata_offset(m, id);
        uint64_t val = off + offsetof(art_hdr_t, val);
        if (off % 8 || val % 8 || (val & 63) > 56)
            return (int64_t)id;
    }
    return -1;
}

/* 旧 store 拒绝测试：把 hdr 的 format_version 改成指定值（只测试用）。 */
void kvspaceShmTestSetFormatVersion(kvspace_t *kv, uint32_t v) {
    if (kv && kv->hdr)
        kv->hdr->format_version = v;
}

#endif

/* 同步强制一轮：返回释放条数；-1 = 别人持有；-2 = 超时放弃（零 free）。 */
static int kv_sync(kvspace_t *kv) {
    if (region_map(&kv->r_art, ART_OFF + (size_t)kv->hdr->art_slab_size) != 0)
        return -1;
    /* 别的进程可能扩过 ART slab：本进程 gens 映射同步跟到同一容量。 */
    if (kv->art_meta->total_size != kv->art_total_seen &&
        gens_grow(kv) != 0)
        return -1;
    if (region_map(&kv->r_head, (size_t)kv->hdr->sbo_head_size) != 0)
        return -1;
    return region_map(&kv->r_data, (size_t)kv->hdr->sbo_data_size);
}

/* blockmalloc only checks total_size; grow the file before publishing it. */
static int art_slab_grow(kvspace_t *kv) {
    size_t want = (size_t)kv->hdr->art_slab_size * 2;
    if (ART_OFF + want > kv->r_art.reserve ||
        ftruncate(kv->r_art.fd, (off_t)(ART_OFF + want)) != 0 ||
        region_map(&kv->r_art, ART_OFF + want) != 0)
        return -1;
    kv->hdr->art_slab_size = want;
    kv->art_meta->total_size = want;
    return 0;
}

/* ---- sbo growth (docs/shm-resize.md). Both grow functions run under
   sbo_plock, which excludes sbo_alloc/free/allocated_size in every process. */

/* Free list empty and no room to append. Unlocked calls are a hint only. */
#ifdef BLOCKMALLOC_COUNT_MASK
#define BM_MALLOC_BLOCKS(m) blocks_malloc_count(m)
#define BM_HEAD_SIZE(m)     blocks_head_size(m)
#else
#define BM_MALLOC_BLOCKS(m) ((uint64_t)(m)->malloc_blocks)
#define BM_HEAD_SIZE(m)     ((uint64_t)(m)->sizeof_block_head)
#endif
static bool sbo_pool_full(sbo_meta_t *m) {
    blocks_meta_t *p = sbo_pool(m);
    if (p->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)block_offset(p, BM_MALLOC_BLOCKS(p)) +
                        BM_HEAD_SIZE(p) + p->block_size;
    return next_end > p->total_size;
}

/* Cap at what the block-head width can address: 14-bit or 30-bit ids. */
static int sbo_head_grow(kvspace_t *kv) {
    sbo_meta_t *m = kv->sbo_meta;
    blocks_meta_t *p = sbo_pool(m);
    uint64_t stride = p->sizeof_block_head + p->block_size;
    uint64_t cap = (1ULL << (p->sizeof_block_head == 2 ? 14 : 30)) * stride;
    uint64_t cur = p->total_size, want = cur * 2;
    if (want > cap)
        want = cap;
    if (SBO_HEAD_FIXED + want > kv->r_head.reserve)
        want = (kv->r_head.reserve - SBO_HEAD_FIXED) & ~(uint64_t)7;
    if (want <= cur ||
        ftruncate(kv->r_head.fd, (off_t)(SBO_HEAD_FIXED + want)) != 0 ||
        region_map(&kv->r_head, SBO_HEAD_FIXED + (size_t)want) != 0)
        return -1;
    p->total_size = want;
    m->per_slot_meta = want;
    m->max_blocks = (uint32_t)(want / m->block_stride);
    kv->hdr->sbo_head_size = SBO_HEAD_FIXED + want;
    return 0;
}

/* x64: move the root into a new block, make block 0 the level above with
   slot 0 pointing at it. Existing offsets stay valid (docs 2.2). */
static int sbo_data_grow(kvspace_t *kv) {
    sbo_meta_t *m = kv->sbo_meta;
    uint64_t cur = m->data_size, want = cur * 64;
    if (want > kv->r_data.reserve)
        return -1;
    if (sbo_pool_full(m) && sbo_head_grow(kv) != 0)
        return -1;
    if (ftruncate(kv->r_data.fd, (off_t)want) != 0 ||
        region_map(&kv->r_data, (size_t)want) != 0)
        return -1;
    int64_t nid = blocks_alloc(sbo_pool(m), sbo_pmem(m));
    if (nid < 0)
        return -1;
    sbo_box_t *root = sbo_box(m, 0), *b = sbo_box(m, (int32_t)nid);
    memcpy(b, root, sizeof *b);
    b->parent = 0;
    b->parent_slot = 0;
    b->self_id = (int32_t)nid;
    uint64_t mm = root->child_mask;
    while (mm) {
        int i = __builtin_ctzll(mm);
        mm &= mm - 1;
        sbo_box(m, root->children[i])->parent = (int32_t)nid;
    }
    sbo_lock_t *lk = sbo_plock(m);
    uint8_t old_lvl = b->objlevel;
    if (old_lvl >= 1 && old_lvl <= 3 && lk->hot_id1[old_lvl - 1] == 1)
        lk->hot_id1[old_lvl - 1] = (uint32_t)nid + 1;
    uint8_t lvl = (uint8_t)(old_lvl + 1);
    sbo_box_format(root, lvl, -1, 255, 0);
    root->free_mask = ~1ULL;
    root->child_mask = 1ULL;
    root->states[0] = SBO_BOX;
    root->children[0] = (int32_t)nid;
    uint16_t b_comb = sbo_combined(b);
    root->child_cap[0] = b_comb;
    root->child_max_enc = b_comb;
    root->child_max_count = 1;
    root->max_obj_cap = SBO_MULT_MAX;
    root->hint_child = 0;
    m->data_size = want;
    m->slot_shift = (uint8_t)__builtin_ctzll(want);
    kv->hdr->sbo_data_size = want;
    return 0;
}

/* Pool exhaustion is exact. Data exhaustion is not (sbo_alloc also fails on
   trylock contention), so re-root only after two failures at one capacity. */
static int kv_sbo_grow(kvspace_t *kv, bool failed, uint64_t *seen_data) {
    sbo_meta_t *m = kv->sbo_meta;
    sbo_lock(sbo_plock(m));
    int rc = 0;
    if (sbo_pool_full(m))
        rc = sbo_head_grow(kv);
    else if (failed && m->data_size == *seen_data)
        rc = sbo_data_grow(kv);
    else if (failed)
        *seen_data = m->data_size;
    sbo_unlock(sbo_plock(m));
    return rc;
}

/* A failed sbo_alloc burns one slot per level (box_boundary is not rolled
   back), so check the pool before allocating. */
static uint64_t kv_sbo_alloc(kvspace_t *kv, size_t n) {
    sbo_meta_t *m = kv->sbo_meta;
    uint64_t seen_data = 0;
    for (int attempt = 0; attempt < 8; attempt++) {
        if (attempt && kv_sync(kv) != 0)
            return (uint64_t)-1;
        if (sbo_pool_full(m) && kv_sbo_grow(kv, false, &seen_data) != 0)
            return (uint64_t)-1;
        uint64_t off = sbo_alloc(m, n);
        if (off != (uint64_t)-1)
            return off;
        if (kv_sbo_grow(kv, true, &seen_data) != 0)
            return (uint64_t)-1;
    }
    return (uint64_t)-1;
}

/* Punch before free: once freed, another process may reuse the range. */
static void kv_sbo_free(kvspace_t *kv, uint64_t off) {
#if defined(__linux__)
    /* Linux 用 fallocate PUNCH_HOLE 归还磁盘块；macOS 无该 API（仅空间回收优化），跳过。 */
    uint64_t sz = sbo_allocated_size(kv->sbo_meta, off);
    if (sz >= SBO_PUNCH_MIN) {
        uint64_t pg = (uint64_t)sysconf(_SC_PAGESIZE);
        uint64_t a = (off + pg - 1) & ~(pg - 1), b = (off + sz) & ~(pg - 1);
        if (b > a)
            (void)fallocate(kv->r_data.fd,
                            FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                            (off_t)a, (off_t)(b - a));
    }
#endif
    sbo_free(kv->sbo_meta, off);
}

/* ---- helpers ---- */
static void *art_blk(kvspace_t *kv, int32_t id) {
    if (id < 0)
        return NULL;
    return kv->art_data + blockdata_offset(kv->art_meta, (uint64_t)id);
}
/* Bump-allocate k consecutive blocks; the node spans them (interior heads unused). */
/* blockmalloc packs sizeof_block_head:4 and malloc_blocks:60 into a single u64 word
 * between block_size and used_blocks. Lock-free readers (art_blk / art_id_of) read this
 * word concurrently, so writers holding meta->lock must update the whole word atomically
 * (via blocks_set_malloc_count when available, or whole-word atomic store fallback). */
_Static_assert(offsetof(blocks_meta_t, used_blocks) ==
                   offsetof(blocks_meta_t, block_size) + 2 * sizeof(uint64_t),
               "blockmalloc: sizeof_block_head/malloc_blocks must share one u64 word");
#ifdef BLOCKMALLOC_COUNT_MASK
static inline void bm_set_malloc_blocks(blocks_meta_t *m, uint64_t n) {
    blocks_set_malloc_count(m, n);
}
#else
#define BM_WORD_OFF (offsetof(blocks_meta_t, block_size) + sizeof(uint64_t))
static unsigned bm_count_shift(void) {
    blocks_meta_t t;
    memset(&t, 0, sizeof t);
    t.malloc_blocks = 1;
    uint64_t w;
    memcpy(&w, (const char *)&t + BM_WORD_OFF, sizeof w);
    return w ? (unsigned)__builtin_ctzll(w) : 64u;
}
/* Caller holds m->lock. */
static void bm_set_malloc_blocks(blocks_meta_t *m, uint64_t n) {
    uint64_t *w = (uint64_t *)((char *)m + BM_WORD_OFF);
    const unsigned sh = bm_count_shift();
    const uint64_t mask = ((1ULL << 60) - 1) << sh;
    uint64_t old = __atomic_load_n(w, __ATOMIC_RELAXED);
    __atomic_store_n(w, (old & ~mask) | ((n << sh) & mask), __ATOMIC_RELEASE);
}
#endif
/* Verify whole-word bitfield layout at open time. */
static bool bm_layout_ok(void) {
    blocks_meta_t t;
    memset(&t, 0, sizeof t);
    t.sizeof_block_head = 8;
    t.malloc_blocks = 5;
    t.used_blocks = 7;
    bm_set_malloc_blocks(&t, 0x0123456789ABCDEull);
    return t.sizeof_block_head == 8 && t.malloc_blocks == 0x0123456789ABCDEull &&
           t.block_size == 0 && t.used_blocks == 7;
}
static int64_t art_bump(kvspace_t *kv, uint32_t k) {
    blocks_meta_t *m = kv->art_meta;
    spin_lock(&m->lock);
    uint64_t id = BM_MALLOC_BLOCKS(m);
    if ((uint64_t)block_offset(m, id + k) > m->total_size) {
        spin_unlock(&m->lock);
        return -1;
    }
    setblock_t(m, kv->art_data + block_offset(m, id), 1, -1);
    bm_set_malloc_blocks(m, id + k);
    m->used_blocks += k;
    spin_unlock(&m->lock);
    return (int64_t)id;
}
static int32_t art_balloc(kvspace_t *kv, int t) {
    const uint64_t head = BM_HEAD_SIZE(kv->art_meta);
    const uint64_t stride = head + kv->art_meta->block_size;
    const uint32_t k = (uint32_t)(((uint64_t)art_node_sz(t) +
                                   head + stride - 1) /
                                  stride);
    int64_t id = art_bump(kv, k);
    while (id < 0) {
        if (art_slab_grow(kv) != 0 || gens_grow(kv) != 0)
            return -1;
        id = art_bump(kv, k);
    }
    if (id >= 0) {
        kv_gens_t *g = gens_at(kv, (int32_t)id);
        if (g) {
            /* 新 id 的 gen 取全局 epoch，保证被复用的 id 不会误命中旧 token。 */
            __atomic_store_n(&g->struct_gen,
                             (uint32_t)gen_alloc_epoch(kv), __ATOMIC_RELEASE);
            __atomic_store_n(&g->value_gen, 0u, __ATOMIC_RELEASE);
        }
    }
    return (int32_t)id;
}
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id) {
    return (art_hdr_t *)art_blk(kv, id);
}

/* ---- prefix ---- */
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl) {
    int n = al < bl ? al : bl;
    for (int i = 0; i < n; i++)
        if (a[i] != b[i])
            return i;
    return n;
}

/* ---- child lookup ---- */
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b) {
    art_hdr_t *h = (art_hdr_t *)n;
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = n;
        for (int i = 0; i < (int)h->count; i++)
            if (x->keys[i] == b)
                return x->children[i];
        return -1;
    }
    case ART_N16: {
        art_n16_t *x = n;
        int lo = 0, hi = (int)h->count - 1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (x->keys[m] == b)
                return x->children[m];
            if (x->keys[m] < b)
                lo = m + 1;
            else
                hi = m - 1;
        }
        return -1;
    }
    case ART_N48: {
        art_n48_t *x = n;
        uint8_t idx = x->index[b];
        return idx == 255 ? -1 : x->children[idx];
    }
    case ART_N256: {
        return ((art_n256_t *)n)->children[b];
    }
    }
    return -1;
}

static int32_t art_follow(kvspace_t *kv, int32_t id);

static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard) {
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        if (h->prefix_len) {
            /* s == prefix_len implies d + prefix_len <= klen */
            if (pfx_shared(h->prefix, h->prefix_len, key + d, klen - d) !=
                h->prefix_len)
                return -1;
            d += h->prefix_len;
        }
        if (d == klen)
            return AV_HAS(art_val_load(h)) ? nid : -1;
        if (par)
            *par = nid;
        if (pard)
            *pard = d;
        nid = art_child(kv, h, key[d]);
        d++;
    }
    return -1;
}

/* ---- art_search ---- */
static int32_t art_find2(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                         int32_t *par, int *pard) {
    if (nid < 0 || !key)
        return -1;
    int32_t parent = -1;
    int parent_d = 0;
    int32_t id = art_walk(kv, nid, key, klen, 0, &parent, &parent_d);
    if (par)
        *par = parent;
    if (pard)
        *pard = parent_d;
    return id;
}
static int32_t art_find(kvspace_t *kv, int32_t nid, const uint8_t *key,
                        int klen) {
    return art_find2(kv, nid, key, klen, NULL, NULL);
}

/* Last path '/' or kvlang member '·' (U+00B7, utf-8 C2 B7). */
static int last_key_sep(const uint8_t *key, int klen, int *seplen) {
    int slash = -1, mid = -1;
    for (int i = 0; i < klen; i++) {
        if (key[i] == '/')
            slash = i;
        if (i + 1 < klen && key[i] == 0xC2 && key[i + 1] == 0xB7)
            mid = i;
    }
    if (mid > slash) {
        *seplen = 2;
        return mid;
    }
    if (slash > 0) {
        *seplen = 1;
        return slash;
    }
    *seplen = 0;
    return -1;
}

/* One walk: leaf plus the ancestor covering last '/' or '·' (after that
 * node's prefix). First match: node at the separator, not a deeper unique
 * prefix that swallowed it. */
static int32_t art_find_dir(kvspace_t *kv, int32_t nid, const uint8_t *key,
                            int klen, int last_sep, int seplen, int32_t *dirn,
                            int *dird) {
    int d = 0;
    int32_t dir = -1;
    int dd = 0;
    if (nid < 0 || !key)
        return -1;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        int entry_d = d;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
            if (s != h->prefix_len)
                return -1;
            d += h->prefix_len;
            if (d > klen)
                return -1;
        }
        if (dir < 0 && last_sep > 0 && seplen > 0 &&
            ((entry_d <= last_sep && last_sep < d) ||
             d == last_sep + seplen)) {
            dir = nid;
            dd = d;
        }
        if (d == klen) {
            if (dirn)
                *dirn = dir;
            if (dird)
                *dird = dd;
            return AV_HAS(art_val_load(h)) ? nid : -1;
        }
        nid = art_child(kv, h, key[d]);
        d++;
    }
    return -1;
}
static art_hdr_t *art_search(kvspace_t *kv, int32_t nid, const uint8_t *key,
                             int klen) {
    int32_t id = art_find(kv, nid, key, klen);
    return id < 0 ? NULL : art_hdr(kv, id);
}
static int32_t art_follow(kvspace_t *kv, int32_t id) {
    for (int i = 0; i < 8 && id >= 0; i++) {
        art_hdr_t *h = art_hdr(kv, id);
        if (!h)
            return -1;
        if (h->type != ART_MOVED)
            return id;
        id = (int32_t)AV_OFF(art_val_load(h));
    }
    return -1;
}

/* ---- node create ---- */
static int32_t art_new_leaf(kvspace_t *kv, uint64_t off) {
    int32_t id = art_balloc(kv, ART_N4);
    if (id < 0)
        return -1;
    art_n4_t *x = art_blk(kv, id);
    memset(x, 0, sizeof(*x));
    x->h.type = ART_N4;
    art_val_store(&x->h, AV_MAKE(1, off));
    return id;
}
static int32_t art_new_node(kvspace_t *kv, int t) {
    int32_t id = art_balloc(kv, t);
    if (id < 0)
        return -1;
    void *x = art_blk(kv, id);
    int sz = art_node_sz(t);
    memset(x, 0, sz);
    ((art_hdr_t *)x)->type = (uint8_t)t;
    if (t == ART_N48)
        memset(((art_n48_t *)x)->index, 255, 256);
    if (t == ART_N256) {
        art_n256_t *n = x;
        for (int i = 0; i < 256; i++)
            n->children[i] = -1;
    }
    return id;
}

/* ---- grow ---- */
static int32_t art_grow(kvspace_t *kv, void *on) {
    art_hdr_t *oh = on;
    int nt;
    if (oh->type == ART_N4)
        nt = ART_N16;
    else if (oh->type == ART_N16)
        nt = ART_N48;
    else if (oh->type == ART_N48)
        nt = ART_N256;
    else
        return -1;
    int32_t nid = art_new_node(kv, nt);
    if (nid < 0)
        return -1;
    art_hdr_t *nh = art_hdr(kv, nid);
    nh->prefix_len = oh->prefix_len;
    memcpy(nh->prefix, oh->prefix, oh->prefix_len);
    art_val_store(nh, art_val_load(oh));
    // copy children
    for (int i = 0; i < (int)oh->count; i++) {
        uint8_t b = 0;
        int32_t c = -1;
        switch (oh->type) {
        case ART_N4: {
            art_n4_t *x = on;
            b = x->keys[i];
            c = x->children[i];
            break;
        }
        case ART_N16: {
            art_n16_t *x = on;
            b = x->keys[i];
            c = x->children[i];
            break;
        }
        case ART_N48: {
            art_n48_t *x = on;
            c = x->children[i];
            for (int j = 0; j < 256; j++)
                if (x->index[j] == i) {
                    b = (uint8_t)j;
                    break;
                }
            break;
        }
        }
        switch (nt) {
        case ART_N16: {
            art_n16_t *x = (art_n16_t *)nh;
            x->keys[i] = b;
            x->children[i] = c;
            break;
        }
        case ART_N48: {
            art_n48_t *x = (art_n48_t *)nh;
            x->index[b] = (uint8_t)i;
            x->children[i] = c;
            break;
        }
        case ART_N256: {
            ((art_n256_t *)nh)->children[b] = c;
            break;
        }
        }
    }
    nh->count = oh->count;
    if (nt == ART_N16) {
        art_n16_t *x = (art_n16_t *)nh;
        for (int i = 0; i < (int)nh->count - 1; i++)
            for (int j = i + 1; j < (int)nh->count; j++)
                if (x->keys[i] > x->keys[j]) {
                    uint8_t tk = x->keys[i];
                    x->keys[i] = x->keys[j];
                    x->keys[j] = tk;
                    int32_t tc = x->children[i];
                    x->children[i] = x->children[j];
                    x->children[j] = tc;
                }
    }
    {   /* P2: 搬迁 —— 新节点 struct_gen = 旧 +1；旧节点也动一次 */
        int32_t oid = art_id_of(kv, oh);
        gen_bump_struct(kv, nid);   /* 新节点 gen 先抬一代 */
        gen_bump_struct(kv, oid);   /* 旧节点也失效 */
    }
    oh->type = ART_MOVED;
    art_val_store(oh, AV_MAKE(0, (uint32_t)nid));
    return nid;
}

/* ---- add child ---- */
static int art_add(kvspace_t *kv, void *n, uint8_t b, int32_t cid) {
    gen_bump_struct(kv, art_id_of(kv, n)); /* P2: 子集变化 */
    art_hdr_t *h = n;
    switch (h->type) {
    case ART_N4: {
        if (h->count >= 4)
            return -1;
        art_n4_t *x = n;
        x->keys[h->count] = b;
        x->children[h->count] = cid;
        h->count++;
        return 0;
    }
    case ART_N16: {
        if (h->count >= 16)
            return -1;
        art_n16_t *x = n;
        int p = (int)h->count;
        while (p > 0 && x->keys[p - 1] > b) {
            x->keys[p] = x->keys[p - 1];
            x->children[p] = x->children[p - 1];
            p--;
        }
        x->keys[p] = b;
        x->children[p] = cid;
        h->count++;
        return 0;
    }
    case ART_N48: {
        if (h->count >= 48)
            return -1;
        art_n48_t *x = n;
        int s = (int)h->count;
        x->index[b] = (uint8_t)s;
        x->children[s] = cid;
        h->count++;
        return 0;
    }
    case ART_N256: {
        art_n256_t *x = n;
        if (x->children[b] >= 0)
            return -1;
        x->children[b] = cid;
        h->count++;
        return 0;
    }
    }
    return -1;
}

/* ---- remove child ---- */
static void art_rm(kvspace_t *kv, void *n, uint8_t b) {
    art_hdr_t *h = n;
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = n;
        for (int i = 0; i < (int)h->count; i++)
            if (x->keys[i] == b) {
                for (int j = i; j < (int)h->count - 1; j++) {
                    x->keys[j] = x->keys[j + 1];
                    x->children[j] = x->children[j + 1];
                }
                h->count--;
                return;
            }
        break;
    }
    case ART_N16: {
        art_n16_t *x = n;
        int lo = 0, hi = (int)h->count - 1, pos = -1;
        while (lo <= hi) {
            int m = (lo + hi) / 2;
            if (x->keys[m] == b) {
                pos = m;
                break;
            }
            if (x->keys[m] < b)
                lo = m + 1;
            else
                hi = m - 1;
        }
        if (pos < 0)
            return;
        for (int j = pos; j < (int)h->count - 1; j++) {
            x->keys[j] = x->keys[j + 1];
            x->children[j] = x->children[j + 1];
        }
        h->count--;
        break;
    }
    case ART_N48: {
        art_n48_t *x = n;
        uint8_t idx = x->index[b];
        if (idx == 255)
            return;
        x->index[b] = 255;
        int last = (int)h->count - 1;
        if (idx != last) {
            x->children[idx] = x->children[last];
            for (int j = 0; j < 256; j++)
                if (x->index[j] == last) {
                    x->index[j] = idx;
                    break;
                }
        }
        h->count--;
        break;
    }
    case ART_N256: {
        art_n256_t *x = n;
        x->children[b] = -1;
        h->count--;
        break;
    }
    }
}

/* ---- 叶子链：key[d..klen) 建链，末端叶子存 off（尾段超 ART_PREFIX_MAX 分级）
 * ---- */
static int32_t art_leaf_chain(kvspace_t *kv, const uint8_t *key, int klen,
                              int d, uint64_t off) {
    int rem = klen - d;
    if (rem <= ART_PREFIX_MAX) {
        int32_t id = art_new_leaf(kv, off);
        if (id < 0)
            return -1;
        art_n4_t *x = art_blk(kv, id);
        if (rem > 0)
            memcpy(x->h.prefix, key + d, (size_t)rem);
        x->h.prefix_len = (uint8_t)rem;
        return id;
    }
    int32_t id = art_new_node(kv, ART_N4);
    if (id < 0)
        return -1;
    art_hdr_t *x = art_hdr(kv, id);
    memcpy(x->prefix, key + d, ART_PREFIX_MAX);
    x->prefix_len = ART_PREFIX_MAX;
    int32_t sub = art_leaf_chain(kv, key, klen, d + ART_PREFIX_MAX + 1, off);
    if (sub < 0)
        return -1;
    art_add(kv, x, key[d + ART_PREFIX_MAX], sub);
    return id;
}

/* ---- insert ---- */
static int32_t art_ins(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                       int d, uint64_t off) {
    if (nid < 0)
        return art_leaf_chain(kv, key, klen, d, off);

    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    int shared = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
    int mpl = h->prefix_len < (klen - d) ? h->prefix_len : (klen - d);

    if (shared == mpl && (klen - d) < h->prefix_len) {
        /* key 在节点 prefix 内部耗尽（含 d==klen）：拆出 shared
         * 字节作新值，旧节点作 child */
        int32_t nn = art_new_node(kv, ART_N4);
        if (nn < 0)
            return -1;
        art_hdr_t *nh = art_hdr(kv, nn);
        nh->prefix_len = (uint8_t)shared;
        if (shared > 0)
            memcpy(nh->prefix, key + d, (size_t)shared);
        uint8_t ob = h->prefix[shared];
        h->prefix_len -= (uint8_t)(shared + 1);
        if (h->prefix_len > 0)
            memmove(h->prefix, h->prefix + shared + 1, h->prefix_len);
        art_add(kv, nh, ob, nid);
        art_val_store(nh, AV_MAKE(1, off));
        return nn;
    }
    if (shared < mpl) { // prefix split
        int32_t nn = art_new_node(kv, ART_N4);
        if (nn < 0)
            return -1;
        art_hdr_t *nh = art_hdr(kv, nn);
        nh->prefix_len = (uint8_t)shared;
        memcpy(nh->prefix, key + d, shared);
        uint8_t ob = h->prefix[shared];
        h->prefix_len -= (uint8_t)(shared + 1);
        if (h->prefix_len > 0)
            memmove(h->prefix, h->prefix + shared + 1, h->prefix_len);
        art_add(kv, nh, ob, nid);
        int32_t leaf = art_leaf_chain(kv, key, klen, d + shared + 1, off);
        if (leaf < 0)
            return -1;
        art_add(kv, nh, key[d + shared], leaf);
        return nn;
    }
    d += h->prefix_len;
    if (d == klen) {
        art_val_store(h, AV_MAKE(1, off));
        return nid;
    }
    int32_t cid = art_child(kv, h, key[d]);
    if (cid >= 0) {
        int32_t nc = art_ins(kv, cid, key, klen, d + 1, off);
        if (nc < 0)
            return -1;
        if (nc != cid) {
            art_rm(kv, h, key[d]);
            art_add(kv, h, key[d], nc);
        }
        return nid;
    }
    int32_t leaf = art_leaf_chain(kv, key, klen, d + 1, off);
    if (leaf < 0)
        return -1;
    if ((h->type == ART_N4 && h->count >= 4) ||
        (h->type == ART_N16 && h->count >= 16) ||
        (h->type == ART_N48 && h->count >= 48)) {
        int32_t g = art_grow(kv, h);
        if (g < 0)
            return -1;
        art_add(kv, art_hdr(kv, g), key[d], leaf);
        return g;
    }
    art_add(kv, h, key[d], leaf);
    return nid;
}

/* ---- delete ---- */
static int32_t art_del(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                       int d, bool *del) {
    if (nid < 0)
        return -1;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    if (h->prefix_len) {
        if (pfx_shared(h->prefix, h->prefix_len, key + d, klen - d) !=
            h->prefix_len)
            return nid;
        d += h->prefix_len;
    }
    if (d == klen) {
        uint64_t hv = art_val_load(h);
        if (!AV_HAS(hv))
            return nid;
        uint64_t gidx;
        if (garb_reserve(kv, &gidx) != 0)
            return -1;                  /* 表满：硬错误，未变更 */
        if (!art_val_cas(h, &hv, 0)) {
            garb_cancel(kv, gidx);
            return nid;
        }
        gen_bump_value(kv, art_id_of(kv, h)); /* P2: 值删 */
        gen_bump_struct(kv, art_id_of(kv, h)); /* P2: 槽消失，view 失效 */
        garb_commit(kv, gidx, AV_OFF(hv),
                    sbo_allocated_size(kv->sbo_meta, AV_OFF(hv)));
        *del = true;
    } else {
        int32_t cid = art_child(kv, h, key[d]);
        if (cid >= 0) {
            int32_t rc = art_del(kv, cid, key, klen, d + 1, del);
            if (rc < 0)
                return rc;
        }
    }
    return nid;
}

/* ---- path ---- */
static char *pjoin(const char *a, const char *b) {
    size_t al = strlen(a), bl = strlen(b);
    int sep = (al > 0 && a[al - 1] != '/') ? 1 : 0;
    char *r = malloc(al + sep + bl + 1);
    memcpy(r, a, al);
    if (sep)
        r[al] = '/';
    memcpy(r + al + sep, b, bl + 1);
    return r;
}
static void psplit(const char *k, char **p, char **n) {
    const char *s = strrchr(k, '/');
    if (!s || s == k) {
        *p = strdup("/");
        *n = strdup(s ? s + 1 : k);
        return;
    }
    size_t pl = (size_t)(s - k) + 1; /* 含尾 '/' */
    *p = malloc(pl + 1);
    memcpy(*p, k, pl);
    (*p)[pl] = '\0';
    *n = strdup(s + 1);
}
/* 非法目录前缀（不是 / 且不以 / 或 · 结尾）→ 返回非 0，对齐 durable
 * validate_dir。 */
static int bad_dir_prefix(const char *p) {
    if (!p || !p[0])
        return 1;
    if (strcmp(p, "/") == 0)
        return 0;
    size_t l = strlen(p);
    if (p[l - 1] == '/')
        return 0;
    if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 &&
        (unsigned char)p[l - 1] == 0xB7)
        return 0;
    return 1;
}

static char *edir(const char *p) {
    size_t l = strlen(p);
    if (l > 0 && p[l - 1] == '/')
        return strdup(p);
    char *r = malloc(l + 2);
    memcpy(r, p, l);
    r[l] = '/';
    r[l + 1] = '\0';
    return r;
}
/* Covering node for pfx: *bpos is reconstructed length at arrival. */
static int32_t art_cover(kvspace_t *kv, int32_t nid, const uint8_t *pfx,
                         int plen, char *buf, int bcap, int *bpos) {
    int d = 0;
    if (nid < 0 || !pfx || plen < 0 || !buf || !bpos)
        return -1;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h)
            return -1;
        int pd = d, cover = 0;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len, pfx + d, plen - d);
            if (s < h->prefix_len) {
                if (d + s < plen)
                    return -1;
                cover = 1;
            } else
                d += h->prefix_len;
        }
        if (cover || d == plen) {
            if (pd >= bcap)
                return -1;
            if (pd)
                memcpy(buf, pfx, (size_t)pd);
            *bpos = pd;
            return nid;
        }
        nid = art_child(kv, h, pfx[d]);
        if (nid < 0)
            return -1;
        d++;
    }
    return -1;
}

typedef struct {
    char **keys;
    int32_t count;
    int32_t cap;
} keyscan_t;

static void keyscan_free(keyscan_t *scan) {
    for (int32_t i = 0; i < scan->count; i++)
        free(scan->keys[i]);
    free(scan->keys);
    scan->keys = NULL;
    scan->count = scan->cap = 0;
}

static int keyscan_add_n(keyscan_t *scan, const char *key, size_t len) {
    if (scan->count == INT32_MAX)
        return -1;
    if (scan->count == scan->cap) {
        int32_t cap = scan->cap > INT32_MAX / 2 ? INT32_MAX :
                      scan->cap ? scan->cap * 2 : 64;
        char **keys = realloc(scan->keys, (size_t)cap * sizeof *keys);
        if (!keys)
            return -1;
        scan->keys = keys;
        scan->cap = cap;
    }
    char *copy = strndup(key, len);
    if (!copy)
        return -1;
    scan->keys[scan->count++] = copy;
    return 0;
}

/* Direct children end at '/' or U+00B7. */
static int child_name_len(const char *rest, int restlen) {
    for (int i = 0; i < restlen; i++) {
        if (rest[i] == '/')
            return i;
        if (i + 1 < restlen && (unsigned char)rest[i] == 0xC2 &&
            (unsigned char)rest[i + 1] == 0xB7)
            return i;
    }
    return restlen;
}

/* Deleted values leave ART nodes behind. */
static int art_has_live_value(kvspace_t *kv, int32_t nid) {
    if (nid < 0)
        return 0;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
        return -1;
    if (AV_HAS(art_val_load(h)))
        return 1;
    const int32_t *children;
    int count = h->count;
    switch (h->type) {
    case ART_N4:
        children = ((art_n4_t *)h)->children;
        break;
    case ART_N16:
        children = ((art_n16_t *)h)->children;
        break;
    case ART_N48:
        children = ((art_n48_t *)h)->children;
        break;
    case ART_N256:
        children = ((art_n256_t *)h)->children;
        count = 256;
        break;
    default:
        return 0;
    }
    for (int i = 0; i < count; i++) {
        int live = art_has_live_value(kv, children[i]);
        if (live != 0)
            return live;
    }
    return 0;
}

static int art_scan_children(kvspace_t *kv, int32_t nid, char *buf, int bpos, int bcap,
                             int plen, keyscan_t *scan) {
    if (nid < 0)
        return 0;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h || h->prefix_len > bcap - bpos - 1)
        return -1;
    /* Recheck the last parent byte for a split U+00B7. */
    int start = bpos > plen + 1 ? bpos - plen - 2 : 0;
    for (int i = 0; i < h->prefix_len; i++)
        buf[bpos++] = h->prefix[i];
    buf[bpos] = 0;
    if (bpos > plen) {
        const char *rest = buf + plen;
        int restlen = bpos - plen;
        int len = start + child_name_len(rest + start, restlen - start);
        /* Stop at the child boundary. */
        if (len < restlen) {
            if (len == 0)
                return 0;
            int live = art_has_live_value(kv, nid);
            if (live <= 0)
                return live;
            if (rest[len] == '/' && len + 1 == restlen &&
                AV_HAS(art_val_load(h)))
                len++;
            return keyscan_add_n(scan, rest, (size_t)len);
        }
        if (AV_HAS(art_val_load(h)) &&
            keyscan_add_n(scan, rest, (size_t)len) != 0)
            return -1;
    }
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = (art_n4_t *)h;
        for (int i = 0; i < (int)h->count; i++) {
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = x->keys[i];
            if (art_scan_children(kv, x->children[i], buf, bpos + 1, bcap,
                                  plen, scan) != 0)
                return -1;
        }
        break;
    }
    case ART_N16: {
        art_n16_t *x = (art_n16_t *)h;
        for (int i = 0; i < (int)h->count; i++) {
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = x->keys[i];
            if (art_scan_children(kv, x->children[i], buf, bpos + 1, bcap,
                                  plen, scan) != 0)
                return -1;
        }
        break;
    }
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->index[i] == 255)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (uint8_t)i;
            if (art_scan_children(kv, x->children[x->index[i]], buf, bpos + 1,
                                  bcap, plen, scan) != 0)
                return -1;
        }
        break;
    }
    case ART_N256: {
        art_n256_t *x = (art_n256_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->children[i] < 0)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (uint8_t)i;
            if (art_scan_children(kv, x->children[i], buf, bpos + 1, bcap,
                                  plen, scan) != 0)
                return -1;
        }
        break;
    }
    }
    return 0;
}

static int art_scan_pfx(kvspace_t *kv, const char *pfx, int plen, char *buf,
                        int bcap, keyscan_t *scan) {
    if (plen >= bcap)
        return -1;
    int bpos = 0;
    int32_t nid = art_cover(kv, kv->hdr->art_root, (const uint8_t *)pfx,
                            plen, buf, bcap, &bpos);
    if (nid < 0)
        return 0;
    return art_scan_children(kv, nid, buf, bpos, bcap, plen, scan);
}

/* Collect all full keys with values under nid (buf[0, bpos) holds the reconstructed prefix).
 * Unlike art_scan_children, does not truncate at child boundaries or hide system names (used by DelTree). */
static int art_collect_keys(kvspace_t *kv, int32_t nid, char *buf, int bpos,
                            int bcap, keyscan_t *scan) {
    if (nid < 0)
        return 0;
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h || h->prefix_len > bcap - bpos - 1)
        return -1;
    for (int i = 0; i < h->prefix_len; i++)
        buf[bpos++] = h->prefix[i];
    if (AV_HAS(art_val_load(h)) && keyscan_add_n(scan, buf, (size_t)bpos) != 0)
        return -1;
    const uint8_t *keys = NULL;
    const int32_t *children = NULL;
    int n = 0;
    switch (h->type) {
    case ART_N4:
        keys = ((art_n4_t *)h)->keys;
        children = ((art_n4_t *)h)->children;
        n = h->count;
        break;
    case ART_N16:
        keys = ((art_n16_t *)h)->keys;
        children = ((art_n16_t *)h)->children;
        n = h->count;
        break;
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->index[i] == 255)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (char)i;
            if (art_collect_keys(kv, x->children[x->index[i]], buf, bpos + 1,
                                 bcap, scan) != 0)
                return -1;
        }
        return 0;
    }
    case ART_N256: {
        art_n256_t *x = (art_n256_t *)h;
        for (int i = 0; i < 256; i++) {
            if (x->children[i] < 0)
                continue;
            if (bpos + 1 >= bcap)
                return -1;
            buf[bpos] = (char)i;
            if (art_collect_keys(kv, x->children[i], buf, bpos + 1, bcap,
                                 scan) != 0)
                return -1;
        }
        return 0;
    }
    default:
        return 0;
    }
    for (int i = 0; i < n; i++) {
        if (bpos + 1 >= bcap)
            return -1;
        buf[bpos] = (char)keys[i];
        if (art_collect_keys(kv, children[i], buf, bpos + 1, bcap, scan) != 0)
            return -1;
    }
    return 0;
}

/* Collect all keys with values starting with pfx (including system variables). */
static int art_scan_keys(kvspace_t *kv, const char *pfx, int plen,
                         keyscan_t *scan) {
    char buf[4096];
    if (plen >= (int)sizeof buf)
        return -1;
    int bpos = 0;
    int32_t nid = art_cover(kv, kv->hdr->art_root, (const uint8_t *)pfx, plen,
                            buf, (int)sizeof buf, &bpos);
    if (nid < 0)
        return 0;
    return art_collect_keys(kv, nid, buf, bpos, (int)sizeof buf, scan);
}

/* ============ lifecycle ============ */
/* 8 * 64^k; s wraps to 0. */
static bool sbo_data_size_ok(size_t n) {
    for (uint64_t s = 8; s; s *= 64)
        if (s == n)
            return true;
    return false;
}

kvspace_t *kvspaceShmOpen(const char *path, size_t data_size) {
    if (!path)
        return NULL;
    if (!bm_layout_ok()) {
        fprintf(stderr, "kvspace: unexpected blockmalloc blocks_meta_t layout\n");
        return NULL;
    }
    char head_path[PATH_MAX], data_path[PATH_MAX], gen_path[PATH_MAX],
        garb_path[PATH_MAX];
    if (snprintf(head_path, sizeof head_path, "%s.sbo.head", path) >=
            (int)sizeof head_path ||
        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path ||
        snprintf(gen_path, sizeof gen_path, "%s.gen", path) >=
            (int)sizeof gen_path ||
        snprintf(garb_path, sizeof garb_path, "%s.garb", path) >=
            (int)sizeof garb_path)
        return NULL;

    kvspace_t *kv = calloc(1, sizeof(*kv));
    if (!kv)
        return NULL;
    kv->p2_slot = -1;
    kv->r_art.fd = kv->r_head.fd = kv->r_data.fd = kv->r_gen.fd =
        kv->r_garb.fd = -1;

    /* validate before O_EXCL create: no empty file left behind */
    bool created = false;
    kv->r_art.fd = open(path, O_RDWR);
    if (kv->r_art.fd < 0) {
        if (!sbo_data_size_ok(data_size))
            goto fail;
        kv->r_art.fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
        if (kv->r_art.fd < 0)
            goto fail;
        created = true;
    }

    size_t art_slab, sbo_head;
    if (created) {
        art_slab = ART_SLAB_INIT;
        sbo_head = sbo_meta_size(data_size, SBO_HEAD_POOL_INIT);
    } else {
        kvspace_hdr_t tmp;
        if (pread(kv->r_art.fd, &tmp, sizeof tmp, 0) != (ssize_t)sizeof tmp ||
            memcmp(tmp.magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1) != 0)
            goto fail;
        /* Check format_version before region_attach: header size changes across versions,
         * so an older store would otherwise fail in region_attach without printing a diagnostic. */
        if (tmp.format_version != KVSPACE_FORMAT_VERSION) {
            fprintf(stderr, "kvspace: unsupported store format (version=%u, "
                            "expected %u): %s; rebuild the shm store\n",
                    tmp.format_version, KVSPACE_FORMAT_VERSION, path);
            goto fail;
        }
        art_slab = (size_t)tmp.art_slab_size;
        sbo_head = (size_t)tmp.sbo_head_size;
        data_size = (size_t)tmp.sbo_data_size;
    }

    /* O_TRUNC: sbo_init rejects a stale magic */
    int fl = created ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    if (region_attach(&kv->r_art, created, ART_OFF + art_slab,
                      ART_RESERVE) != 0 ||
        (kv->r_head.fd = open(head_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_head, created, sbo_head, REGION_RESERVE) != 0 ||
        (kv->r_data.fd = open(data_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_data, created, data_size, DATA_RESERVE) != 0 ||
        (kv->r_gen.fd = open(gen_path, fl, 0644)) < 0 ||
        (kv->r_garb.fd = open(garb_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_garb, created,
                      (size_t)GARB_CAP * sizeof(garb_ent_t),
                      (size_t)GARB_CAP * sizeof(garb_ent_t)) != 0)
        goto fail;

    kv->hdr = (kvspace_hdr_t *)kv->r_art.base;
    kv->art_meta = (blocks_meta_t *)(kv->r_art.base + sizeof(kvspace_hdr_t));
    kv->art_data = kv->r_art.base + ART_OFF;
    kv->sbo_meta = (sbo_meta_t *)kv->r_head.base;
    kv->sbo_data = kv->r_data.base;
    kv->garb = (garb_ent_t *)kv->r_garb.base;

    /* 每进程单调句柄序号：close 后重开即使 malloc 复用同一 kv 地址，旧 token 也失效。 */
    {
        static uint32_t store_seq;
        kv->store_id = ((uint64_t)getpid() << 32) | (uint64_t)++store_seq;
    }

    if (created) {
        memset(kv->hdr, 0, sizeof(*kv->hdr));
        kv->hdr->format_version = KVSPACE_FORMAT_VERSION;
        kv->hdr->garb_cap = GARB_CAP;
        kv->hdr->art_slab_size = art_slab;
        kv->hdr->sbo_head_size = sbo_head;
        kv->hdr->sbo_data_size = data_size;
        kv->hdr->art_root = -1;
        if (blocks_init(kv->art_meta, art_slab, ART_BLOCK_SZ) != 0 ||
            sbo_init(kv->sbo_meta, sbo_head, data_size) != 0)
            goto fail;
        /* blocks_init 按容量选 2/4/8 字节块头。4 字节头 + 8 对齐的块大小使
         * 节点地址交替落在 8k+4，art_hdr_t.val（偏移 16）跨 64B 行：x86
         * locked CAS 变成 split lock（实测 ~7k tick/次，del p95 26k）。
         * 节点地址必须 8 字节对齐（原子字不可跨行），故强制 8 字节头。 */
        kv->art_meta->sizeof_block_head = 8;
        if ((kv->art_meta->sizeof_block_head +
             kv->art_meta->block_size) % 8 != 0)
            goto fail;
        /* magic last: half-initialized file fails reopen */
        memcpy(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1);
    }
    if (!created &&
        (kv->hdr->format_version != KVSPACE_FORMAT_VERSION ||
         kv->art_meta->sizeof_block_head != 8 ||
         kv->hdr->garb_cap != GARB_CAP)) {
        fprintf(stderr, "kvspace: unsupported store format (version=%u, "
                        "garb_cap=%llu); rebuild the shm store\n",
                kv->hdr->format_version, (unsigned long long)kv->hdr->garb_cap);
        goto fail;
    }
    if (gens_grow(kv) != 0)   /* P2: gen 表在 blocks_init 之后 */
        goto fail;

    /* growth code assumes the single-root pool layout (SBO_HEAD_FIXED) */
    if (kv->sbo_meta->root_slots != 1)
        goto fail;

    /* P2: 注册表认领。占满（N=64）拒绝 open，错误明确，不静默扩容。 */
    kv->p2_slot = p2_reg_claim(kv);
    if (kv->p2_slot == -2) {
        fprintf(stderr, "kvspace: too many processes, N=%d\n", P2_SLOT_N);
        goto fail;
    }
    if (kv->p2_slot == -3) {
        fprintf(stderr, "kvspace: claim_id exhausted (2^30 opens): "
                        "rebuild the store\n");
        goto fail;
    }
    if (kv->p2_slot < 0)
        goto fail;
    kv->pid = (uint32_t)getpid();
    (void)pthread_once(&g_atfork_once, p2_atfork_install);
    kv->fork_gen = __atomic_load_n(&g_fork_gen, __ATOMIC_ACQUIRE);
    kv->quiesced = 1;               /* claim 时 slot->quiesced=1，二者一致 */
    if (kv->hdr->committed_bytes -
            __atomic_load_n(&kv->hdr->freed_bytes, __ATOMIC_ACQUIRE) >=
            p2_garb_threshold() &&
        p2_maint_start(kv) != 0)
        goto fail;                  /* 已有大量垃圾且建不了线程：open 硬失败 */

    return kv;

fail:
    p2_maint_stop(kv);
    if (kv->p2_slot >= 0 && kv->hdr)
        p2_reg_release(kv);
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_gen);
    region_close(&kv->r_garb);
    free(kv);
    if (created) {
        unlink(path);
        unlink(head_path);
        unlink(data_path);
        unlink(gen_path);
        unlink(garb_path);
    }
    return NULL;
}
/* 除自己外是否还有存活进程（macOS 无 /proc：只有 FREE 槽才算“不在”）。 */
static bool p2_other_alive(kvspace_t *kv) {
    for (int i = 0; i < P2_SLOT_N; i++) {
        if (i == kv->p2_slot)
            continue;
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
            continue;
        if (p2_slot_alive(s))
            return true;
    }
    return false;
}

void kvspaceShmClose(kvspace_t *kv) {
    if (!kv)
        return;
    /* 最后一个存活进程负责清场：Linux 跑一次机会性 round；macOS 直接清环。 */
    if (kv->pid == (uint32_t)getpid() && kv->p2_slot >= 0 &&
        !p2_other_alive(kv)) {
#if defined(__linux__)
        __atomic_store_n(&kv->hdr->slots[kv->p2_slot].quiesced, 1u,
                         __ATOMIC_RELEASE);
        (void)p2_round(kv);
#else
        uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
        uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_ACQUIRE);
        for (; h < t; h++) {
            garb_ent_t *e = &kv->garb[h & (GARB_CAP - 1)];
            uint32_t m = __atomic_load_n(&e->meta, __ATOMIC_ACQUIRE);
            if (GE_STATE(m) == GE_COMMITTED) {
                uint64_t off = __atomic_load_n(&e->off, __ATOMIC_ACQUIRE);
                uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
                __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
                kv_sbo_free(kv, off);
                __atomic_store_n(&kv->hdr->freed_bytes,
                                 kv->hdr->freed_bytes + cap, __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(&e->meta, GE_EMPTY, __ATOMIC_RELEASE);
            }
        }
        __atomic_store_n(&kv->hdr->garb_head, t, __ATOMIC_RELEASE);
#endif
    }
    p2_maint_stop(kv);
    p2_reg_release(kv);
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_gen);
    region_close(&kv->r_garb);
    free(kv);
}

/* ---- link resolve helpers ---- */
/* Parse full value length = 16 + bodylen + attrslen within [s, s+bound)。
   head_of 已校验总长落在 bound 内且 padding 为零，长度不再有第二个来源。 */
static int parse_value(const uint8_t *s, uint64_t bound, int32_t *ol) {
    if (bound > INT32_MAX)
        bound = INT32_MAX;
    kvspaceXvalueHead wire;
    if (head_of(s, (int32_t)bound, &wire) != 0)
        return -1;
    const uint64_t total = kvspaceXvalueHeadTotalLen(&wire);
    if (total == 0 || total > bound)
        return -1;
    *ol = (int32_t)total;
    return 0;
}

/* Write path: bound by physical box capacity (takes allocator lock) so a corrupted
 * header cannot cause caller writes to overflow into adjacent boxes. */
static int read_value(kvspace_t *kv, uint64_t off, uint8_t **out, int32_t *ol) {
    *out = kv->sbo_data + off;
    return parse_value(*out, sbo_allocated_size(kv->sbo_meta, off), ol);
}

/* Read-only path: bound by mapped data region without taking the allocator lock.
 * sbo_allocated_size contends on the cross-process spinlock on every read; since value_ok
 * already ensures 8+len+attr_len fits in the box at write time, both bounds agree on valid values. */
static uint64_t ro_bound(const kvspace_t *kv, uint64_t off) {
    uint64_t m = kv->r_data.mapped;
    return off < m ? m - off : 0;
}
static int read_value_ro(kvspace_t *kv, uint64_t off, uint8_t **out,
                         int32_t *ol) {
    *out = kv->sbo_data + off;
    return parse_value(*out, ro_bound(kv, off), ol);
}
/* Link resolution: ptr (ref=1) and @ext (ref=2) XValues are always leaves (never intermediate
 * `/` prefixes), so only the full leaf key can be a link. Follow at most 16 hops, fetching
 * the terminal leaf value at the same time (fetched=1) to avoid a second art_search + read_value.
 * Missing/valueless leaf returns fetched=0 for extindex/dir fallback. Caller ensures strlen(path) < osz. */
static art_hdr_t *resolve_fetch(kvspace_t *kv, const char *path, char *out, int osz,
                                uint8_t **raw, int32_t *rl, int *fetched) {
    *fetched = 0;
    strncpy(out, path, osz - 1);
    out[osz - 1] = '\0';
    for (int depth = 0; depth < 16; depth++) {
        art_hdr_t *h =
            art_search(kv, kv->hdr->art_root, (const uint8_t *)out, (int)strlen(out));
        if (!h)
            return h;
        uint64_t hv = art_val_load(h);
        if (!AV_HAS(hv))
            return h;
        uint8_t *s = kv->sbo_data + AV_OFF(hv);
        uint64_t sz = ro_bound(kv, AV_OFF(hv));
        kvspaceXvalueHead wire;
        if (head_of(s, sz > INT32_MAX ? INT32_MAX : (int32_t)sz, &wire) == 0) {
            uint64_t l3 = kvspaceXvalueHeadLen(&wire);
            if (kvspace_c_reference_of(s, sz) != KVSPACE_XVALUE_REFERENCE_PTR ||
                l3 >= (uint64_t)osz) {
                if (parse_value(s, sz, rl) != 0)
                    return NULL;
                *raw = s;
                *fetched = 1;
                return h;
            }
            memcpy(out, s + KVSPACE_XVALUE_HEAD_LEN, (size_t)l3);
            out[l3] = 0;
            continue;
        }
        return NULL;
    }
    return NULL;
}

/* Resolve path only (used by List). */
static void resolve_path(kvspace_t *kv, const char *path, char *out, int osz) {
    uint8_t *raw;
    int32_t rl;
    int fetched;
    (void)resolve_fetch(kv, path, out, osz, &raw, &rl, &fetched);
}

/* Read the extension locator stored at a directory key. */
static int dir_ext_path(kvspace_t *kv, const char *dir, char *out, int osz) {
    out[0] = 0;
    art_hdr_t *h =
        art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)strlen(dir));
    if (!h)
        return 0;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 0;
    uint8_t *raw;
    int32_t rl;
    if (read_value_ro(kv, AV_OFF(hv), &raw, &rl) < 0)
        return 0;
    kvspaceXvalueHead wire;
    if (head_of(raw, rl, &wire) != 0 ||
        kvspace_c_reference_of(raw, (uint64_t)rl) != KVSPACE_XVALUE_REFERENCE_EXT)
        return 0;
    uint64_t l4 = kvspaceXvalueHeadLen(&wire);
    if (l4 == 0 || l4 >= (uint64_t)osz)
        return 0;
    memcpy(out, raw + KVSPACE_XVALUE_HEAD_LEN, (size_t)l4);
    out[l4] = 0;
    return 1;
}

/* ============ CRUD ============ */
uint8_t *kvspaceShmGet(kvspace_t *kv, const char *key, int resolve,
                       int32_t *ol) {
    if (!kv || !key || !ol)
        return NULL;
    *ol = 0;
    if (reserved_meta_path(key))
        return NULL;
    if (kv_sync(kv) != 0)
        return NULL;
    char kbuf[1024];
    /* Set rejects >= 1024-byte keys; do not truncate and look up a shorter key. */
    if (strlen(key) >= sizeof kbuf)
        return NULL;
    uint8_t *raw;
    int32_t rl;
    int fetched = 0;
    art_hdr_t *h;
    if (resolve)
        h = resolve_fetch(kv, key, kbuf, sizeof(kbuf), &raw, &rl, &fetched);
    else {
        strncpy(kbuf, key, sizeof(kbuf) - 1);
        kbuf[sizeof(kbuf) - 1] = '\0';
        h = art_search(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                       (int)strlen(kbuf));
    }
    uint64_t hv = h ? art_val_load(h) : 0;
    if (!h || !AV_HAS(hv)) {
        /* extindex fallback：父目录是 extindex → 读 extpath + name */
        char *parent = NULL, *name = NULL;
        psplit(kbuf, &parent, &name);
        char extpath[1024];
        if (dir_ext_path(kv, parent, extpath, sizeof extpath)) {
            char *target = pjoin(extpath, name);
            art_hdr_t *eh = art_search(kv, kv->hdr->art_root, (const uint8_t *)target,
                                       (int)strlen(target));
            free(target);
            uint64_t ehv = eh ? art_val_load(eh) : 0;
            if (eh && AV_HAS(ehv)) {
                uint8_t *raw;
                int32_t rl;
                if (read_value_ro(kv, AV_OFF(ehv), &raw, &rl) == 0) {
                    *ol = rl;
                    free(parent);
                    free(name);
                    return raw;
                }
            }
        }
        free(parent);
        free(name);
        return NULL;
    }
    if (!fetched && read_value_ro(kv, AV_OFF(hv), &raw, &rl) < 0)
        return NULL;
    *ol = rl;
    return raw;
}

/* Partial write by key (follows links and extindex fallback like Get with resolve=1).
 * Locates via lock-free read, then re-bounds by physical box capacity before writing. */
int kvspaceShmSetPart(kvspace_t *kv, const char *key, uint32_t offset,
                      const uint8_t *buf, uint32_t buf_len) {
    if (!kv || !key || (buf_len && !buf))
        return -1;
    int32_t rl = 0;
    uint8_t *d = kvspaceShmGet(kv, key, 1, &rl);
    uint64_t bound = 0U;
    if (!d || read_value(kv, (uint64_t)(d - kv->sbo_data), &d, &rl) != 0 ||
        kvspace_c_effective_len(d, (uint64_t)rl, &bound) != 0 ||
        (uint64_t)offset + buf_len > (uint64_t)KVSPACE_XVALUE_HEAD_LEN + bound)
        return -1;
    memcpy(d + offset, buf, buf_len);
    return 0;
}

/* /vthread/<vid>/‥status：合法内容长度上限与创建时物理容量下限。 */
#define KVSPACE_STATUS_MAX_LEN 16u
#define KVSPACE_STATUS_MIN_CAP 80u

static int is_vthread_status_key(const char *key) {
    static const char pfx[] = "/vthread/";
    static const char suf[] = "/\xE2\x80\xA5status";
    if (!key)
        return 0;
    size_t n = strlen(key), sl = sizeof suf - 1;
    return n > sl && strncmp(key, pfx, sizeof pfx - 1) == 0 &&
           memcmp(key + n - sl, suf, sl) == 0;
}

/* langtype 的 base kind 是不是 char：`char/X`、`[N]char/X`、`[]char/X` 都算。
   只看第一个 `]` 之后（形状段可以有多个维度）。引用前缀不在此判——引用槽在
   调用点已被拒绝。 */
static int is_char_langtype(const uint8_t *langtype, uint16_t length) {
    if (!langtype || length == 0)
        return 0;
    uint16_t at = 0;
    if (langtype[0] == '[') {
        while (at < length && langtype[at] != ']')
            ++at;
        if (at >= length)
            return 0;
        ++at;
    }
    return (uint16_t)(length - at) >= 5 &&
           memcmp(langtype + at, "char/", 5) == 0;
}

/* Raw local write (internal helper without container/member semantics).
 * reserve: minimum physical box capacity (for WriteNewPlace body_cap); 0 uses val_len. */
static int shm_set_local(kvspace_t *kv, const char *key, const uint8_t *val,
                         int32_t val_len, art_hdr_t *old, size_t reserve) {
    size_t alloc_len = (size_t)val_len;
    if (reserve > alloc_len)
        alloc_len = reserve;
    if (is_vthread_status_key(key) && alloc_len < KVSPACE_STATUS_MIN_CAP)
        alloc_len = KVSPACE_STATUS_MIN_CAP;
    uint64_t ohv = old ? art_val_load(old) : 0;
    bool retiring = old && AV_HAS(ohv);
    uint64_t gidx = 0;
    uint64_t cap = 0;
    if (retiring) {
        cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(ohv));
        if ((uint64_t)alloc_len <= cap) {
            /* Existing box fits (including reserve): write in place under value_gen seqlock */
            kv_gens_t *g = gens_at(kv, art_id_of(kv, old));
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            memcpy(kv->sbo_data + AV_OFF(ohv), val, (size_t)val_len);
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            return 0;
        }
        /* 换 box：reserve 在任何变更之前；失败 = 硬错误、旧值原样 */
        if (garb_reserve(kv, &gidx) != 0)
            return -1;
#ifdef P2_TEST_HOOKS
        {
            const char *v = getenv("P2_TEST_HOLD_RESERVE_MS");
            if (v && atoi(v) > 0)
                usleep((useconds_t)atoi(v) * 1000);
        }
        if (getenv("P2_TEST_FAIL_ALLOC")) {
            garb_cancel(kv, gidx);
            return -1;
        }
#endif
    }
    uint64_t off = kv_sbo_alloc(kv, alloc_len);
    if (off == (uint64_t)-1) {
        if (retiring)
            garb_cancel(kv, gidx);
        return -1;
    }
    memcpy(kv->sbo_data + off, val, val_len);
#ifdef P2_TEST_HOOKS
    if (retiring && getenv("P2_TEST_FAIL_ARTINS")) {
        garb_cancel(kv, gidx);
        kv_sbo_free(kv, off);
        return -1;
    }
#endif
    int32_t root = art_ins(kv, kv->hdr->art_root, (const uint8_t *)key,
                           (int)strlen(key), 0, off);
    if (root < 0) {
        if (retiring)
            garb_cancel(kv, gidx);
        kv_sbo_free(kv, off);          /* 新 box 从未被引用 */
        return -1;
    }
    kv->hdr->art_root = root;
    {   /* 换 box / 墓碑复活：旧 token 全部 STALE */
        int32_t nid = p2_node_at(kv, key);
        kv_gens_t *g = gens_at(kv, nid);
        if (g) {
            gen_bump_struct(kv, nid);
            gen_bump_value(kv, nid);
        }
    }
    if (retiring)
        garb_commit(kv, gidx, AV_OFF(ohv), cap);
    else
        list_gen_bump(kv);   /* new key or tombstone revive: bump after art_ins */
    return 0;
}

static int shm_set_raw(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len, size_t reserve) {
    art_hdr_t *old = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                                (int)strlen(key));
    return shm_set_local(kv, key, val, val_len, old, reserve);
}

/* 「这个写入目标是 vthread 帧的操作数、且它是一个 Ptr」。
 * 旧模型读 `wire->ptr`；新模型没有该字段，引用性由 langtype 前缀表达，
 * 所以这里要多一层 wire 字节才能判——多出的两个参数就是为此。 */
static int frame_operand_ptr(const char *key, const uint8_t *wire_bytes,
                             uint64_t wire_len) {
    if (kvspace_c_reference_of(wire_bytes, wire_len) !=
            KVSPACE_XVALUE_REFERENCE_PTR ||
        strncmp(key, "/vthread/", 9) != 0)
        return 0;
    const char *slot = strrchr(key, '/');
    if (!slot || slot[1] != '[')
        return 0;
    char *end;
    long row = strtol(slot + 2, &end, 10);
    if (row <= 0 || *end++ != ',')
        return 0;
    long col = strtol(end, &end, 10);
    return col != 0 && *end++ == ']' && *end == '\0';
}

static int shm_set_validated(kvspace_t *kv, const char *key,
                            const uint8_t *val, int32_t val_len,
                            const kvspaceXvalueHead *wire) {
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
    if (strlen(key) >= sizeof kbuf || strstr(key, "//") ||
        reserved_meta_path(key))
        return -1;
    strcpy(kbuf, key);

    art_hdr_t *local = art_search(kv, kv->hdr->art_root,
                                   (const uint8_t *)kbuf, (int)strlen(kbuf));
    /* Existing local values already shadow the extension. */
    uint64_t lhv = local ? art_val_load(local) : 0;
    if (!local || !AV_HAS(lhv)) {
        char *pp = NULL, *nn = NULL;
        psplit(kbuf, &pp, &nn);
        char extpath[1024];
        if (pp && nn && dir_ext_path(kv, pp, extpath, sizeof extpath)) {
            char *tgt = pjoin(extpath, nn);
            art_hdr_t *eh = art_search(kv, kv->hdr->art_root,
                                       (const uint8_t *)tgt, (int)strlen(tgt));
            free(tgt);
            if (eh && AV_HAS(art_val_load(eh)) &&
                !frame_operand_ptr(kbuf, val, (uint64_t)val_len)) {
                free(pp);
                free(nn);
                return -1;
            }
        }
        free(pp);
        free(nn);
    }

    /* status slot: **文本形态**（langtype 以 `char/` 开头）+ 长度 <= 16 + 物理容量足够。
     *
     * 旧模型用 `storetype == SLACK` 表达「这是一个变长文本槽」。新布局没有
     * storetype 位，而「变长文本」在新模型里恰好有**明确表达**：langtype 是
     * 字符种类（`char/utf8` / `[N]char/…`）。所以判据从「storetype 枚举」换成
     * 「langtype 是不是字符种类」——这不是找替代物，是把隐含信息显式化：
     * 状态槽的语义本来就是「一段短文本」。
     *
     * 只判长度会放过 `int64` 这类定宽标量写进 status 槽（旧代码靠 storetype
     * 拦住它们），那是**放宽**，不是迁移。 */
    if (is_vthread_status_key(kbuf)) {
        const uint8_t *status_langtype = NULL;
        const uint16_t status_langtype_len =
            kvspaceXvalueHeadLangtypeBounded(val, (uint64_t)val_len,
                                             &status_langtype);
        uint64_t effective = 0;
        if (kvspaceXvalueHeadEffectiveLen(val, (uint64_t)val_len, &effective) != 0 ||
            effective > KVSPACE_STATUS_MAX_LEN ||
            !is_char_langtype(status_langtype, status_langtype_len))
            return -1;
        if (local && AV_HAS(lhv)) {
            uint8_t *old_raw = kv->sbo_data + AV_OFF(lhv);
            uint64_t old_sz = sbo_allocated_size(kv->sbo_meta, AV_OFF(lhv));
            kvspaceXvalueHead old;
            /* 旧 store 的不合规 status 槽（形状/容量任一不符）：
             * 不自动升级、不 realloc——写入硬错，需重建/重置。 */
            const uint8_t *old_langtype = NULL;
            const uint16_t old_langtype_len =
                kvspaceXvalueHeadLangtypeBounded(old_raw, (uint64_t)old_sz,
                                                 &old_langtype);
            uint64_t old_effective = 0;
            if (head_of(old_raw, (int32_t)old_sz, &old) != 0 ||
                kvspaceXvalueHeadEffectiveLen(old_raw, (uint64_t)old_sz,
                                              &old_effective) != 0 ||
                old_effective > KVSPACE_STATUS_MAX_LEN ||
                old_sz < KVSPACE_STATUS_MIN_CAP ||
                !is_char_langtype(old_langtype, old_langtype_len))
                return -1;
        }
    }
    return shm_set_local(kv, kbuf, val, val_len, local, 0);
}

int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                  int32_t val_len) {
    if (!kv || !key || !val || val_len <= 0)
        return -1;
    kvspaceXvalueHead wire;
    if (value_ok(val, val_len, &wire) != 0)
        return -1;
    return shm_set_validated(kv, key, val, val_len, &wire);
}

int kvspaceShmSetValue(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len) {
    kvspaceXvalueHead head;
    if (!kv || !key || !val || val_len <= 0 ||
        value_ok(val, val_len, &head) != 0)
        return -2;
    return shm_set_validated(kv, key, val, val_len, &head);
}

/* Delete a single key directly (without following links) and without bumping
 * list_gen. Returns 1 = deleted, 0 = missing, -1 = hard error (tree unchanged).
 * Caller has called kv_sync and verified strlen(key) < 1024. */
static int shm_del_key(kvspace_t *kv, const char *key) {
    bool d = false;
    int32_t root = art_del(kv, kv->hdr->art_root, (const uint8_t *)key,
                           (int)strlen(key), 0, &d);
    if (root < 0)
        return -1;
    kv->hdr->art_root = root;
    return d ? 1 : 0;
}

int kvspaceShmDel(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    if (strlen(key) >= 1024)
        return -1;                  /* Reject instead of truncating into another key */
    /* Delete the key itself without following links; explicit deref belongs to runtime */
    if (shm_del_key(kv, key) != 1)
        return -1;
    list_gen_bump(kv);              /* Key disappeared: bump after CAS */
    return 0;
}

/* 成员目录 marker（p·，U+00B7）。 */
static char *memdir(const char *p) {
    size_t l = strlen(p);
    char *r = malloc(l + 3);
    memcpy(r, p, l);
    r[l] = (char)0xC2;
    r[l + 1] = (char)0xB7;
    r[l + 2] = 0;
    return r;
}

/* Whether key belongs to base's subtree: base itself, or base followed by '/'
 * (hierarchy), U+00B7 (member), or U+2025 (runtime/attr key). Empty base = root
 * (all keys starting with '/'). Matches kvspace-durable store::is_subtree_key. */
static int subtree_key(const char *base, size_t bl, const char *key) {
    if (bl == 0)
        return key[0] == '/';
    if (strncmp(key, base, bl) != 0)
        return 0;
    const unsigned char *r = (const unsigned char *)key + bl;
    return r[0] == 0 || r[0] == '/' || (r[0] == 0xC2 && r[1] == 0xB7) ||
           (r[0] == 0xE2 && r[1] == 0x80 && r[2] == 0xA5);
}

static int is_ptr_key(kvspace_t *kv, const char *key) {
    art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key));
    uint64_t hv = h ? art_val_load(h) : 0;
    if (!h || !AV_HAS(hv))
        return 0;
    uint8_t *raw = NULL;
    int32_t rl = 0;
    kvspaceXvalueHead wire;
    return read_value(kv, AV_OFF(hv), &raw, &rl) == 0 && rl > 0 &&
           head_of(raw, rl, &wire) == 0 &&
           kvspace_c_reference_of(raw, (uint64_t)rl) ==
               KVSPACE_XVALUE_REFERENCE_PTR;
}

/* Delete the entire subtree under prefix. A single trailing '/' or U+00B7 on
 * prefix normalizes to base ("/d/" and "/d<U+00B7>" both delete "/d"; "/" is root).
 * Deletes base, base/..., base<U+00B7>..., and base<U+2025>... (including hidden
 * U+2025 runtime keys). Does not follow links (POSIX rm semantics): if prefix
 * or base is a pointer, only the pointer is deleted. */
int kvspaceShmDeltree(kvspace_t *kv, const char *prefix) {
    if (!kv || !prefix || reserved_meta_path(prefix))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    size_t pl = strlen(prefix);
    if (pl >= 1024)
        return -1;                  /* Reject instead of truncating into another prefix */
    char base[1024];
    memcpy(base, prefix, pl + 1);
    size_t bl = pl;
    if (strcmp(prefix, "/") == 0)
        bl = 0;
    else if (bl && base[bl - 1] == '/')
        bl--;
    else if (bl >= 2 && (unsigned char)base[bl - 2] == 0xC2 &&
             (unsigned char)base[bl - 1] == 0xB7)
        bl -= 2;
    base[bl] = 0;
    if (bl && reserved_meta_path(base))
        return -1;
    /* prefix itself is a link: delete only the link. */
    if (pl && is_ptr_key(kv, prefix))
        return kvspaceShmDel(kv, prefix);
    if (bl && bl != pl && is_ptr_key(kv, base)) {
        int rc = kvspaceShmDel(kv, base);
        kvspaceShmDel(kv, prefix);  /* Companion key such as "/p/", may not exist */
        return rc;
    }
    keyscan_t keys = {0};
    if (art_scan_keys(kv, bl ? base : "/", bl ? (int)bl : 1, &keys) != 0) {
        keyscan_free(&keys);
        return -1;
    }
    int rc = 0, any = 0;
    for (int32_t i = 0; i < keys.count; i++) {
        const char *k = keys.keys[i];
        if (!subtree_key(base, bl, k) || reserved_meta_path(k) ||
            strlen(k) >= 1024)
            continue;
        int d = shm_del_key(kv, k);
        if (d < 0)
            rc = -1;
        else if (d)
            any = 1;
    }
    keyscan_free(&keys);
    if (any)
        list_gen_bump(kv);          /* Keys disappeared: bump once after batch delete */
    return rc;
}

/* Copy one value before Set can move its slab. */
int kvspaceShmCp(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst)
        return -1;
    int32_t rl;
    uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
    if (!raw || rl <= 0)
        return -1;
    uint8_t *tmp = malloc((size_t)rl);
    if (!tmp)
        return -1;
    memcpy(tmp, raw, (size_t)rl);
    int rc = kvspaceShmSet(kv, dst, tmp, rl);
    free(tmp);
    return rc;
}

/* Copy values and physical children recursively. */
static int cptree_rec(kvspace_t *kv, const char *src, const char *dst) {
    {
        int32_t rl;
        uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
        if (raw && rl > 0) {
            if (kvspaceShmCp(kv, src, dst) != 0)
                return -1;
        }
    }
    char *es = edir(src), *ed = edir(dst);
    char **ns;
    int32_t nc;
    if (kvspaceShmList(kv, es, false, 1, &ns, &nc) != 0) {
        free(es);
        free(ed);
        return -1;
    }
    int rc = 0;
    for (int i = 0; i < nc; i++) {
        char *cs = pjoin(es, ns[i]), *cd = pjoin(ed, ns[i]);
        if (!cs || !cd || cptree_rec(kv, cs, cd) != 0)
            rc = -1;
        free(cs);
        free(cd);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < nc; i++)
        free(ns[i]);
    free(ns);
    free(es);
    free(ed);
    if (rc != 0)
        return -1;

    char *ms = memdir(src), *md = memdir(dst);
    char **mms;
    int32_t mc;
    if (kvspaceShmList(kv, ms, false, 1, &mms, &mc) != 0) {
        free(ms);
        free(md);
        return -1;
    }
    for (int i = 0; i < mc; i++) {
        size_t msl = strlen(ms), mdl = strlen(md), nl = strlen(mms[i]);
        char *cs = malloc(msl + nl + 1);
        char *cd = malloc(mdl + nl + 1);
        if (!cs || !cd) {
            rc = -1;
        } else {
            memcpy(cs, ms, msl);
            memcpy(cs + msl, mms[i], nl + 1);
            memcpy(cd, md, mdl);
            memcpy(cd + mdl, mms[i], nl + 1);
            if (cptree_rec(kv, cs, cd) != 0)
                rc = -1;
        }
        free(cs);
        free(cd);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < mc; i++)
        free(mms[i]);
    free(mms);
    free(ms);
    free(md);
    return rc;
}

int kvspaceShmCptree(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst || reserved_meta_path(src) ||
        reserved_meta_path(dst))
        return -1;
    size_t sl = strlen(src);
    while (sl > 1 && src[sl - 1] == '/')
        sl--;
    if (sl == 1 && src[0] == '/' && dst[0] == '/')
        return dst[1] ? -1 : 0;
    if (strncmp(src, dst, sl) == 0) {
        if (dst[sl] == 0 || (dst[sl] == '/' && dst[sl + 1] == 0))
            return 0;
        if (dst[sl] == '/' ||
            ((unsigned char)dst[sl] == 0xc2 &&
             (unsigned char)dst[sl + 1] == 0xb7))
            return -1;
    }
    kvspaceShmDeltree(kv, dst);
    return cptree_rec(kv, src, dst);
}

/* Copy the base value and direct member values. */
int kvspaceShmCplist(kvspace_t *kv, const char *src, const char *dst) {
    if (!kv || !src || !dst || reserved_meta_path(src) ||
        reserved_meta_path(dst))
        return -1;
    if (strcmp(src, dst) == 0)
        return 0;
    kvspaceShmDeltree(kv, dst);
    int32_t rl;
    uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
    if (raw && rl > 0 && kvspaceShmCp(kv, src, dst) != 0)
        return -1;
    char *ms = memdir(src), *md = memdir(dst);
    char **mms;
    int32_t mc;
    if (kvspaceShmList(kv, ms, false, 1, &mms, &mc) != 0) {
        free(ms);
        free(md);
        return -1;
    }
    size_t msl = strlen(ms), mdl = strlen(md);
    int rc = 0;
    for (int i = 0; i < mc; i++) {
        size_t nl = strlen(mms[i]);
        char *cs = malloc(msl + nl + 1);
        char *cd = malloc(mdl + nl + 1);
        if (!cs || !cd) {
            rc = -1;
        } else {
            memcpy(cs, ms, msl);
            memcpy(cs + msl, mms[i], nl + 1);
            memcpy(cd, md, mdl);
            memcpy(cd + mdl, mms[i], nl + 1);
            if (kvspaceShmCp(kv, cs, cd) != 0)
                rc = -1;
        }
        free(cs);
        free(cd);
        if (rc != 0)
            break;
    }
    for (int i = 0; i < mc; i++)
        free(mms[i]);
    free(mms);
    free(ms);
    free(md);
    return rc;
}

int kvspaceShmMkindex(kvspace_t *kv, const char *path, uint32_t capacity) {
    if (!kv || !path)
        return -1;
    (void)capacity;
    char *d = edir(path);
    int32_t existing_len = 0;
    if (kvspaceShmGet(kv, d, 0, &existing_len)) {
        free(d);
        return 0;
    }
    uint8_t *v = NULL;
    uint64_t vl = 0;
    int r = kvspaceXvalueHeadNewNone(&v, &vl);
    if (r == 0)
        r = kvspaceShmSetValue(kv, d, v, (int32_t)vl);
    free(v);
    free(d);
    return r;
}

static int child_cmp(const void *a, const void *b) {
    const char *x = *(const char *const *)a;
    const char *y = *(const char *const *)b;
    size_t nx = strlen(x), ny = strlen(y);
    char *tx = nx && x[nx - 1] == '/' ? strndup(x, nx - 1) : NULL;
    char *ty = ny && y[ny - 1] == '/' ? strndup(y, ny - 1) : NULL;
    int c = kvspaceCoordCmp(tx ? tx : x, ty ? ty : y);
    free(tx);
    free(ty);
    return c ? c : strcmp(x, y);
}

static int same_child(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    if (na && a[na - 1] == '/') na--;
    if (nb && b[nb - 1] == '/') nb--;
    return na == nb && memcmp(a, b, na) == 0;
}

/* Resolve List input: prefix -> target directory (following links when resolve=1)
 * and mounted extindex source ext (when ex=1 and mounted; "" otherwise).
 * Returns 0 = scan, 1 = reserved path (empty result), -1 = error.
 * target buffer >= 4096 bytes, ext buffer >= 1024 bytes. */
static int list_target(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                       char *target, size_t tcap, char *ext, size_t ecap) {
    ext[0] = 0;
    if (reserved_meta_path(prefix))
        return 1;
    if (kv_sync(kv) != 0 || bad_dir_prefix(prefix))
        return -1;
    if (resolve) {
        if (strlen(prefix) >= 1024)
            return -1;              /* Reject instead of truncating into another dir */
        resolve_path(kv, prefix, target, 1024);
    } else {
        size_t l = strlen(prefix);
        if (l >= tcap)
            return -1;
        memcpy(target, prefix, l + 1);
    }
    if (reserved_meta_path(target))
        return 1;
    if (ex) {
        char *dir = edir(target);
        if (!dir)
            return -1;
        if (!dir_ext_path(kv, dir, ext, (int)ecap))
            ext[0] = 0;
        free(dir);
    }
    return 0;
}
/* Scan direct children of target (and mounted ext): sort, deduplicate (prefer
 * directory entry on name collision), and hide runtime entries. */
static int list_scan(kvspace_t *kv, const char *pfx, const char *ext,
                     char ***on, int32_t *oc) {
    keyscan_t children = {0};
    char buf[4096];
    if (kv->hdr->art_root >= 0 &&
        art_scan_pfx(kv, pfx, (int)strlen(pfx), buf, (int)sizeof buf, &children) != 0)
        goto fail;
    if (ext[0] && art_scan_pfx(kv, ext, (int)strlen(ext), buf, (int)sizeof buf,
                               &children) != 0)
        goto fail;
    if (children.count > 1)
        qsort(children.keys, (size_t)children.count, sizeof *children.keys,
              child_cmp);
    int32_t unique = 0;
    for (int32_t i = 0; i < children.count; i++) {
        if (strcmp(pfx, "/") == 0 &&
            strcmp(children.keys[i], ".kvspace-meta") == 0) {
            free(children.keys[i]);
            continue;
        }
        /* Child names containing U+2025 (\xE2\x80\xA5) are runtime system keys
           (e.g. status, pc); hide from List to match the durable backend. */
        if (strstr(children.keys[i], "\xE2\x80\xA5")) {
            free(children.keys[i]);
            continue;
        }
        if (unique && same_child(children.keys[unique - 1], children.keys[i])) {
            if (children.keys[i][strlen(children.keys[i]) - 1] == '/') {
                free(children.keys[unique - 1]);
                children.keys[unique - 1] = children.keys[i];
                continue;
            }
            free(children.keys[i]);
            continue;
        }
        children.keys[unique++] = children.keys[i];
    }
    children.count = unique;
    *on = children.keys;
    *oc = children.count;
    return 0;
fail:
    keyscan_free(&children);
    return -1;
}
int kvspaceShmList(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                   char ***on, int32_t *oc) {
    if (!kv || !prefix || !on || !oc)
        return -1;
    *on = NULL;
    *oc = 0;
    char target[4096], ext[1024];
    int rc = list_target(kv, prefix, ex, resolve, target, sizeof target, ext,
                         sizeof ext);
    if (rc != 0)
        return rc > 0 ? 0 : -1;
    return list_scan(kv, target, ext, on, oc);
}
/* 直接扫描，不复用上一次结果：调用方每次拿到的都是当次 key 集合。 */
int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                      int32_t *out_count) {
    if (!out_count)
        return -1;
    *out_count = 0;
    if (!kv || !prefix)
        return -1;
    char target[4096], ext[1024];
    int rc = list_target(kv, prefix, ex, resolve, target, sizeof target, ext,
                         sizeof ext);
    if (rc != 0)
        return rc > 0 ? 0 : -1;
    char **names = NULL;
    int32_t count = 0;
    if (list_scan(kv, target, ext, &names, &count) != 0)
        return -1;
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    *out_count = count;
    return 0;
}
int kvspaceShmExtindex(kvspace_t *kv, const char *p, const char *ep,
                       char *err, uint32_t err_cap) {
    if (!kv || !p || !ep)
        return -1;
    /* Match durable ext_index: path and ext_path must be directory keys ('/' or U+00B7 suffix). */
    if (bad_dir_prefix(p) || bad_dir_prefix(ep)) {
        if (err && err_cap)
            snprintf(err, err_cap,
                     "kvspace: index must end with /: ExtIndex path=%s extpath=%s",
                     p, ep);
        return -1;
    }
    int32_t source_len = 0;
    uint8_t *source = kvspaceShmGet(kv, ep, 0, &source_len);
    if (!source || source_len <= 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "ExtIndex target missing: %s", ep);
        return -1;
    }
    /* source 类型在 ep 值 attr 区（明文事实源）；ext 句柄同样携带。 */
    char lt[256];
    const uint8_t *ltd = NULL;
    uint16_t ltl = 0;
    if (source_len >= (int32_t)KVSPACE_XVALUE_HEAD_LEN) {
        kvspaceXvalueHead sh;
        memcpy(&sh, source, sizeof sh);
        /* 合法性只剩「head 自洽」：`TotalLen` 一并校验 padding 与总长。 */
        if (kvspaceXvalueHeadTotalLen(&sh) != 0) {
            const uint64_t stotal = kvspaceXvalueHeadTotalLen(&sh);
            if (stotal != 0 && stotal <= (uint64_t)source_len)
                ltl = kvspaceXvalueHeadLangtype(source, &ltd);
        }
    }
    if (ltl > (uint16_t)(sizeof lt - 1))
        ltl = (uint16_t)(sizeof lt - 1);
    if (ltl)
        memcpy(lt, ltd, ltl);
    lt[ltl] = 0;
    uint8_t *v = NULL;
    uint64_t vl = 0;
    int r = kvspaceXvalueHeadNewExt((const uint8_t *)ep, (uint32_t)strlen(ep),
                                    lt, &v, &vl);
    if (r == 0)
        r = kvspaceShmSetValue(kv, p, v, (int32_t)vl);
    free(v);
    return r;
}
int kvspaceShmDelextindex(kvspace_t *kv, const char *p) {
    return kvspaceShmDel(kv, p);
}

/* ============ 存储层 ABI 原语（<kvspace/kvspace.h>） ============
 *
 * 原语只被本文件的 12 个公共函数使用，直接返回 KVSPACE_*，没有第二套
 * 内部状态码，也没有映射层。 */

/* 与旧指针式接口的差别只有三条，都是 kvspace.h 的契约：
 *   1. 定位只按 key，不跟 Ptr（旧 Get(resolve=1) 会跟）；
 *   2. 不认 extindex（旧 List 会展开挂载源）；
 *   3. 返回**偏移**而非指针（调用方用「本进程基址 + 偏移」自己读写）。
 *
 * 写原语统一走 box 生命周期（garb_reserve/garb_commit，与旧 shm_set_local 同源），
 * 只是把「调用方先备好整条 wire」换成「先给容量，再由调用方填 body」。 */

/* 定位 key 的**值**节点；不跟 Ptr、不跟 ext。 */
static int storage_locate(kvspace_t *kv, const char *key, int32_t *out_id,
                          uint64_t *out_offset) {
    art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key));
    if (!h)
        return KVSPACE_MISSING;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return KVSPACE_MISSING;
    *out_id = art_id_of(kv, h);
    *out_offset = AV_OFF(hv);
    return KVSPACE_OK;
}

/* 公共 int64 偏移 → 引擎 anchor 结构。偏移必须落在 ART key 区内、且能反算
 * 到合法块号；任何越界/错位都拒绝，不猜。 */
static int anchor_from_offset(kvspace_t *kv, int64_t key_offset,
                              kvspaceAnchor_t *out) {
    if (!kv || !out || key_offset <= 0)
        return -1;
    if ((uint64_t)key_offset >= kv->art_meta->total_size)
        return -1;
    int64_t block_id = blockid_bydataoffset(kv->art_meta, (uint64_t)key_offset);
    if (block_id < 0 || (uint64_t)block_id >= kv->art_meta->used_blocks)
        return -1;
    out->key_offset = key_offset;
    out->block_id = (int32_t)block_id;
    return 0;
}

/* anchor 校验：指向的 ART 节点必须仍在 slab 内、且不是 MOVED。
 *
 * `art_blk` 只挡 id < 0，越界 id 会算到映射之外——所以这里必须先按 slab 的
 * 块数自证 id 在范围内，再取节点。anchor 来自调用方（可能是跨进程传的旧值），
 * 不能假设它可信。
 *
 * **两层哨兵别混**：公共 ABI 的 `anchor == 0` 是 root 哨兵，由 `kvspaceResolve`
 * 在进 `ResolveFrom` 之前就分流掉，根本不会走到这里；到了这里的 `block_id`
 * 是引擎块号，**0 是合法节点**（`art_id_of` 的失败值才是 -1）。曾经这里写
 * `<= 0`，于是空库中落在 block 0 的父节点拿到锚点也被判 ANCHOR_INVALID——
 * 合法值被当成哨兵。 */
static int storage_anchor_ok(kvspace_t *kv, const kvspaceAnchor_t *anchor) {
    if (!kv || !anchor || anchor->block_id < 0)
        return 0;
    if ((uint64_t)anchor->block_id >= kv->art_meta->used_blocks)
        return 0;
    art_hdr_t *h = art_hdr(kv, anchor->block_id);
    if (!h || h->type == ART_MOVED)
        return 0;
    return 1;
}

int kvspaceShmStorageResolveFrom(kvspace_t *kv, const char *key,
                                 kvspaceAnchor_t anchor, uint64_t *out_offset) {
    int32_t id = -1;
    if (!kv || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (reserved_meta_path(key))
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    if (!storage_anchor_ok(kv, &anchor))
        return KVSPACE_ANCHOR_INVALID;
    return storage_locate(kv, key, &id, out_offset);
}

/* 进程内值区基址（地址空间访问器）。地址 = 基址 + value_offset。
 * 基址在进程生命周期内稳定：region_attach 先预留 VA，region_grow 用
 * MAP_FIXED 在同一 VA 上扩，故扩容前后 base 不变。
 * 跨进程只传 offset，不传 base（各进程 VA 不同）。 */
uint8_t *kvspaceShmStorageDataBase(kvspace_t *kv) {
    return kv ? kv->sbo_data : NULL;
}

int kvspaceShmStorageResolve(kvspace_t *kv, const char *key,
                             uint64_t *out_offset) {
    int32_t id = -1;
    if (!kv || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (reserved_meta_path(key))
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    return storage_locate(kv, key, &id, out_offset);
}

#ifdef P2_TEST_HOOKS
/* benchmark-only：已存在父节点 key → 公共 int64 anchor（= ART key 区字节偏移）。
 *
 * 锚点就是 `blockdata_offset(art_meta, block_id)`——与 `anchor_from_offset` 的
 * 反算互逆。三条拒绝路径各自对应一种「锚点不可用」：节点不在（MISSING）、
 * 节点存在但值槽为空（MISSING，与 storage_locate 同判据）、反算不出合法块号
 * （BACKEND）。**不造值**：算不出来就失败，让调用方看到真实状态。 */
int kvspaceShmTestAnchorOf(kvspace_t *kv, const char *parent_key,
                           int64_t *out_anchor) {
    if (!kv || !parent_key || !out_anchor)
        return KVSPACE_INVALID;
    /* 不在入口清零：失败时调用方看到的是「没被写过」而不是「写了个 0」。
     * 0 是合法偏移语义里的根锚点，用它当失败标志会把两件事混成一件。 */
    if (reserved_meta_path(parent_key))
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    art_hdr_t *h = art_search(kv, kv->hdr->art_root,
                              (const uint8_t *)parent_key,
                              (int)strlen(parent_key));
    if (!h || !AV_HAS(art_val_load(h)))
        return KVSPACE_MISSING;
    /* 块号 0 合法（`art_id_of` 的失败值是 -1），故判 `id < 0` 而非 `id <= 0`。 */
    int32_t id = art_id_of(kv, h);
    if (id < 0)
        return KVSPACE_BACKEND;
    if ((uint64_t)id >= kv->art_meta->used_blocks)
        return KVSPACE_BACKEND;
    if (art_hdr(kv, id)->type == ART_MOVED)
        return KVSPACE_BACKEND;
    int64_t offset = (int64_t)blockdata_offset(kv->art_meta, (uint64_t)id);
    if (offset <= 0)
        return KVSPACE_BACKEND;
    *out_anchor = offset;
    return KVSPACE_OK;
}
#endif

/* 写 head（bodylen/attrslen/padding）。attr 与 body 的内容是调用方的。 */
static void storage_write_head(uint8_t *dst, uint64_t body_len,
                               uint64_t attr_len) {
    kvspaceXvalueHead h;
    memset(&h, 0, sizeof h);
    kvspaceXvalueHeadSetLen(&h, body_len);
    kvspaceXvalueHeadSetAttrsLen(&h, (uint16_t)attr_len);
    memcpy(dst, &h, sizeof h);
}

/* 唯一写原语的内核：不存在则创建（清零）；存在则容量内原地、超容量搬移。
 * 返回 KVSPACE_*，成功时 *out_offset 是新偏移。 */
int kvspaceShmStorageRealloc(kvspace_t *kv, const char *key,
                             uint64_t body_len, uint64_t attr_len,
                             uint64_t *out_offset) {
    if (!kv || !key || !out_offset)
        return KVSPACE_INVALID;
    *out_offset = 0;
    if (reserved_meta_path(key) || strstr(key, "//") ||
        strlen(key) >= 1024)
        return KVSPACE_INVALID;
    if (attr_len > UINT16_MAX)
        return KVSPACE_INVALID;
    uint64_t need = KVSPACE_XVALUE_HEAD_LEN + body_len;
    if (need < body_len || need > UINT64_MAX - attr_len)
        return KVSPACE_INVALID;
    need += attr_len;
    if (need > INT32_MAX)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;

    art_hdr_t *node = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                                 (int)strlen(key));
    uint64_t ohv = node ? art_val_load(node) : 0;
    bool retiring = node && AV_HAS(ohv);
    uint64_t gidx = 0;
    uint64_t old_cap = 0;
    if (retiring) {
        uint64_t off = AV_OFF(ohv);
        old_cap = sbo_allocated_size(kv->sbo_meta, off);
        if (need <= old_cap) {
            /* 容量内原地：只重写 head，body/attr 由调用方按新 head 覆盖。
               旧偏移继续有效——这正是契约允许的「原地」分支。 */
            kv_gens_t *g = gens_at(kv, art_id_of(kv, node));
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            storage_write_head(kv->sbo_data + off, body_len, attr_len);
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            *out_offset = off;
            return KVSPACE_OK;
        }
        if (garb_reserve(kv, &gidx) != 0)
            return KVSPACE_BACKEND;
    }
    uint64_t off = kv_sbo_alloc(kv, (size_t)need);
    if (off == (uint64_t)-1) {
        if (retiring)
            garb_cancel(kv, gidx);
        return KVSPACE_BACKEND;
    }
    /* 新建与搬移都清零：契约写明「不存在则创建清零」，搬移也走同一条路径，
       省掉「新盒子里有上一手残留」这种只在一半用例里出现的行为。
       清零之后立刻写 head——body/attr 的内容是调用方的，但 head 是本原语的
       职责，漏写就会读出一份全零的 head。 */
    memset(kv->sbo_data + off, 0, (size_t)need);
    storage_write_head(kv->sbo_data + off, body_len, attr_len);
    int32_t root = art_ins(kv, kv->hdr->art_root, (const uint8_t *)key,
                           (int)strlen(key), 0, off);
    if (root < 0) {
        if (retiring)
            garb_cancel(kv, gidx);
        kv_sbo_free(kv, off);
        return KVSPACE_BACKEND;
    }
    kv->hdr->art_root = root;
    {
        int32_t nid = p2_node_at(kv, key);
        kv_gens_t *g = gens_at(kv, nid);
        if (g) {
            gen_bump_struct(kv, nid);
            gen_bump_value(kv, nid);
        }
    }
    if (retiring)
        garb_commit(kv, gidx, AV_OFF(ohv), old_cap);
    else
        list_gen_bump(kv);
    *out_offset = off;
    return KVSPACE_OK;
}

/* 是否索引 key：尾 `/` 或 U+00B7（`·`）。目录身份 = key 形状，不靠元数据。 */
static int storage_is_index_key(const char *key) {
    size_t n = strlen(key);
    if (n == 0)
        return 0;
    if (key[n - 1] == '/')
        return 1;
    return n >= 2 && (unsigned char)key[n - 2] == 0xC2 &&
           (unsigned char)key[n - 1] == 0xB7;
}

/* 该索引是否还有直接物理子项（不看 extindex）。 */
static int storage_has_children(kvspace_t *kv, const char *key) {
    keyscan_t children = {0};
    char buf[4096];
    int rc = 0;
    if (kv->hdr->art_root < 0)
        return KVSPACE_OK;
    if (art_scan_pfx(kv, key, (int)strlen(key), buf, (int)sizeof buf,
                     &children) != 0) {
        keyscan_free(&children);
        return KVSPACE_BACKEND;
    }
    if (children.count > 0)
        rc = KVSPACE_HAS_CHILDREN;
    keyscan_free(&children);
    return rc;
}

int kvspaceShmStorageFree(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return KVSPACE_INVALID;
    if (reserved_meta_path(key) || strlen(key) >= 1024)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    /* 索引：有子项就拒（ENOTEMPTY），空索引连节点一起删。 */
    if (storage_is_index_key(key)) {
        char *d = edir(key);
        int child_rc = storage_has_children(kv, d);
        free(d);
        if (child_rc != KVSPACE_OK)
            return child_rc;
    }
    int rc = shm_del_key(kv, key);
    if (rc < 0)
        return KVSPACE_BACKEND;
    if (rc == 0)
        return KVSPACE_MISSING;
    list_gen_bump(kv);
    return KVSPACE_OK;
}

/* 建 index 位置：key 必须尾 `/` 或 `·`；幂等；只保证位置存在，body/langtype
 * 由上层用 realloc 填。无 capacity 参数。 */
int kvspaceShmStorageMakeindex(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return KVSPACE_INVALID;
    if (reserved_meta_path(key) || strlen(key) >= 1024 ||
        strstr(key, "//"))
        return KVSPACE_INVALID;
    if (!storage_is_index_key(key))
        return KVSPACE_INVALID;
    uint64_t off = 0;
    int rc = kvspaceShmStorageResolve(kv, key, &off);
    if (rc == KVSPACE_OK)
        return KVSPACE_OK; /* 幂等：已存在即成功，不覆盖上层写过的 body */
    if (rc != KVSPACE_MISSING)
        return rc;
    /* 新尺度：0 字节 body + 0 attr —— 上层随后用 realloc 写 body/langtype。 */
    rc = kvspaceShmStorageRealloc(kv, key, 0, 0, &off);
    return rc;
}

/* key（两种形态都查）是否还有 live value：
 * 带尾 `/` 的目录形态，或去掉尾分隔符的值形态。deltree 两种都会删，
 * 所以存在性判断必须两种都看。 */
/* 判「这个 key 存不存在」，**按 key 形态分开问**：
 *
 *   - 索引形态（尾 `/` 或 `·`）：问的是「这里有没有一个目录」。有根值、
 *     **或者有后代**，都算存在——`/only/child/` 存在就意味着 `/only/` 这个
 *     目录存在（即便它自己没有根值）。目录身份由 key 形状决定，不靠根值。
 *   - 值形态（无尾分隔符）：问的是「这里有没有一个值」。只查 key 自身与其
 *     目录形态；**不把后代算作存在**——`/only` 与 `/only/` 是两个不同的 key
 *     形态，前者没有值就是 NOT_INDEX，不能借着后者的后代说自己存在。
 *
 * 这个区分是 `resolve` 与 `listlen` 语义不同的根源：`resolve` 问值（索引无根值
 * 时 MISSING 是对的），`listlen` 问子项（有后代就该 OK）。 */
static int storage_key_exists(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return 0;
    art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key));
    if (h && AV_HAS(art_val_load(h)))
        return 1;

    if (storage_is_index_key(key)) {
        /* 索引形态：有后代即存在（空索引由调用方按 OK/0/0 处理）。 */
        keyscan_t children = {0};
        char buf[4096];
        int rc = 0;
        if (kv->hdr->art_root >= 0)
            rc = art_scan_pfx(kv, key, (int)strlen(key), buf, (int)sizeof buf,
                              &children);
        int has = rc == 0 && children.count > 0;
        keyscan_free(&children);
        return has;
    }

    /* 值形态：再看同名目录形态有没有根值（deltree 两种都删）。 */
    size_t n = strlen(key);
    char dir[1024];
    if (n == 0 || n + 2 > sizeof dir)
        return 0;
    memcpy(dir, key, n);
    dir[n] = '/';
    dir[n + 1] = 0;
    h = art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)strlen(dir));
    return h && AV_HAS(art_val_load(h));
}

/* 三态判据（listlen 与 list 共用），顺序不可交换：
 *   1. key 不存在（按形态问，见 `storage_key_exists`）⇒ MISSING
 *   2. key 存在但不是索引形态                        ⇒ NOT_INDEX
 *   3. 其余交给 storage_children（空索引 ⇒ OK/0/0）
 *
 * **先存在性、后形态**：`listlen("/never")` 既不存在的 key 也不是索引，
 * 报 MISSING 才是对的——「不存在」是更基本的事实，形态判断只对存在的 key 有意义。
 * 反过来先判形态，会让「不存在的普通 key」得到 NOT_INDEX，调用方据此以为
 * 「这个 key 存在但用错了接口」，从而掩盖真正的缺失。
 *
 * 注意值形态的存在性**包含**「它有根值，或它的目录形态有根值」：`/only` 自身
 * 没值、但 `/only/` 有值（或反过来）时，`/only` 这个 key 是存在的，
 * 于是走到这里得到 NOT_INDEX 而不是 MISSING。 */
static int storage_index_state(kvspace_t *kv, const char *key) {
    if (!storage_is_index_key(key))
        return storage_key_exists(kv, key) ? KVSPACE_NOT_INDEX : KVSPACE_MISSING;
    if (!storage_key_exists(kv, key))
        return KVSPACE_MISSING;
    return KVSPACE_OK;
}

/* 直接子项原始列表：后端顺序（不排序），名字按 list 的同一字节串计
 * （目录项带尾 `/`）。只列物理子项，不展开 extindex。
 *
 * **索引不存在 ≠ 空索引**：key 没有 live value 且没有子项 ⇒ MISSING；
 * 只有 key 存在（有值）但没有成员，才是 OK + 空列表。这条判据不靠「扫出来
 * 0 条」推断——ART 删值后节点还在（墓碑），扫出 0 条既可能是空目录，
 * 也可能是整个 key 都不存在，必须回头查 key 本身。 */
static int storage_children(kvspace_t *kv, const char *key, keyscan_t *out) {
    char buf[4096];
    if (kv->hdr->art_root >= 0 &&
        art_scan_pfx(kv, key, (int)strlen(key), buf, (int)sizeof buf, out) != 0) {
        keyscan_free(out);
        return KVSPACE_BACKEND;
    }
    if (out->count == 0 && !storage_key_exists(kv, key))
        return KVSPACE_MISSING;
    return KVSPACE_OK;
}

/* 索引键归一化为枚举前缀（尾 `·` 换成 `·` 本身即可，尾 `/` 原样）。 */
static int storage_list_prefix(const char *key, char *out, size_t cap) {
    size_t n = strlen(key);
    if (n == 0 || n >= cap)
        return KVSPACE_INVALID;
    memcpy(out, key, n + 1);
    return KVSPACE_OK;
}

int kvspaceShmStorageListlen(kvspace_t *kv, const char *key, int64_t *out_count,
                             uint32_t *out_max_name_length) {
    if (!kv || !key || !out_count || !out_max_name_length)
        return KVSPACE_INVALID;
    *out_count = 0;
    *out_max_name_length = 0;
    if (reserved_meta_path(key) || strlen(key) >= 1024)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    int rc = storage_index_state(kv, key);
    if (rc != KVSPACE_OK)
        return rc;
    char pfx[1024];
    rc = storage_list_prefix(key, pfx, sizeof pfx);
    if (rc != KVSPACE_OK)
        return rc;
    keyscan_t children = {0};
    rc = storage_children(kv, pfx, &children);
    if (rc != KVSPACE_OK)
        return rc;
    uint32_t max_len = 0;
    for (int32_t i = 0; i < children.count; i++) {
        size_t n = strlen(children.keys[i]);
        if (n > max_len)
            max_len = (uint32_t)n;
    }
    *out_count = children.count;
    *out_max_name_length = max_len;
    keyscan_free(&children);
    return KVSPACE_OK;
}

int kvspaceShmStorageList(kvspace_t *kv, const char *key, char *out_names,
                          uint32_t out_capacity, uint32_t *out_length,
                          uint32_t *out_count, uint64_t *out_generation) {
    if (!kv || !key)
        return KVSPACE_INVALID;
    if (out_length)
        *out_length = 0;
    if (out_count)
        *out_count = 0;
    if (out_generation)
        *out_generation = 0;
    if (reserved_meta_path(key) || strlen(key) >= 1024)
        return KVSPACE_INVALID;
    if (kv_sync(kv) != 0)
        return KVSPACE_BACKEND;
    int rc = storage_index_state(kv, key);
    if (rc != KVSPACE_OK)
        return rc;
    char pfx[1024];
    rc = storage_list_prefix(key, pfx, sizeof pfx);
    if (rc != KVSPACE_OK)
        return rc;
    keyscan_t children = {0};
    rc = storage_children(kv, pfx, &children);
    if (rc != KVSPACE_OK)
        return rc;
    uint64_t total = 0;
    for (int32_t i = 0; i < children.count; i++)
        total += (uint64_t)strlen(children.keys[i]) + 1;
    if (out_generation)
        *out_generation = __atomic_load_n(&kv->hdr->list_gen, __ATOMIC_ACQUIRE);
    if (out_count)
        *out_count = (uint32_t)children.count;
    if (out_length)
        *out_length = (uint32_t)total;
    /* out_names == NULL：只回报长度/个数/generation。 */
    if (out_names != NULL) {
        if (total > out_capacity) {
            /* 容量不足：仍写出所需值，不截断。 */
            keyscan_free(&children);
            return KVSPACE_CAPACITY;
        }
        char *p = out_names;
        for (int32_t i = 0; i < children.count; i++) {
            size_t n = strlen(children.keys[i]) + 1;
            memcpy(p, children.keys[i], n);
            p += n;
        }
    }
    keyscan_free(&children);
    return KVSPACE_OK;
}


/* ================================================================
 * 公共存储 ABI：<kvspace/kvspace.h>
 *
 * 本文件是 kvspace-c 的参考实现，只暴露头里声明的符号；内部原语
 * （kvspaceShm 系列与 storage 系列）不新增公共面。
 * ================================================================ */

#define KVSPACE_SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

#if defined(__GNUC__)
#define KVSPACE_PUBLIC __attribute__((visibility("default")))
#else
#define KVSPACE_PUBLIC
#endif

static int kvspace_dsn_path(const char *dsn, char *out, size_t out_size) {
    if (!dsn)
        return -1;
    const char *separator = strstr(dsn, "://");
    if (!separator || separator - dsn != 3 || strncmp(dsn, "shm", 3) != 0)
        return -1;
    int written = snprintf(out, out_size, "%s", separator + 3);
    return written > 0 && (size_t)written < out_size ? 0 : -1;
}

static int kvspace_fail(int status, char *err, uint32_t err_cap,
                        const char *operation, const char *key) {
    if (err && err_cap)
        snprintf(err, err_cap, "kvspace: %s failed at %s", operation,
                 key ? key : "(null)");
    return status;
}

KVSPACE_PUBLIC void *kvspaceConnect(const char *dsn) {
    char path[1024];
    if (kvspace_dsn_path(dsn, path, sizeof path) != 0)
        return NULL;
    return (void *)kvspaceShmOpen(path, KVSPACE_SHM_DEFAULT_SIZE);
}

KVSPACE_PUBLIC void kvspaceClose(void *handle) {
    if (handle)
        kvspaceShmClose((kvspace_t *)handle);
}

KVSPACE_PUBLIC uint8_t *kvspaceXvalueInProcessAddress(void *handle) {
    if (!handle)
        return NULL;
    return kvspaceShmStorageDataBase((kvspace_t *)handle);
}

KVSPACE_PUBLIC int kvspaceResolve(void *handle, const char *key,
                                 int64_t parent_key_anchor,
                                 uint64_t *out_value_offset, char *err,
                                 uint32_t err_cap) {
    if (!handle || !key || !out_value_offset)
        return KVSPACE_INVALID;
    *out_value_offset = 0;
    int internal;
    if (parent_key_anchor == KVSPACE_ANCHOR_ROOT) {
        internal = kvspaceShmStorageResolve((kvspace_t *)handle, key,
                                            out_value_offset);
    } else {
        kvspaceAnchor_t anchor;
        if (anchor_from_offset((kvspace_t *)handle, parent_key_anchor,
                               &anchor) != 0)
            return kvspace_fail(KVSPACE_ANCHOR_INVALID, err, err_cap,
                                "resolve", key);
        internal = kvspaceShmStorageResolveFrom((kvspace_t *)handle, key,
                                                anchor, out_value_offset);
    }
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "resolve", key);
}

KVSPACE_PUBLIC int kvspaceRealloc(void *handle, const char *key,
                                 uint64_t body_length, uint64_t attr_length,
                                 uint64_t *out_value_offset, char *err,
                                 uint32_t err_cap) {
    if (!handle || !key || !out_value_offset)
        return KVSPACE_INVALID;
    *out_value_offset = 0;
    int internal = kvspaceShmStorageRealloc((kvspace_t *)handle, key,
                                            body_length, attr_length,
                                            out_value_offset);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "realloc",
                        key);
}

KVSPACE_PUBLIC int kvspaceFree(void *handle, const char *key, char *err,
                               uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = kvspaceShmStorageFree((kvspace_t *)handle, key);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "free", key);
}

KVSPACE_PUBLIC int kvspaceCp(void *handle, const char *old_key,
                             const char *new_key, char *err,
                             uint32_t err_cap) {
    if (!handle || !old_key || !new_key)
        return KVSPACE_INVALID;
    if (kvspaceShmCp((kvspace_t *)handle, old_key, new_key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, err, err_cap, "cp", old_key);
    return KVSPACE_OK;
}

KVSPACE_PUBLIC int kvspaceMkindex(void *handle, const char *key, char *err,
                                  uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = kvspaceShmStorageMakeindex((kvspace_t *)handle, key);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "mkindex",
                        key);
}

KVSPACE_PUBLIC int kvspaceDeltree(void *handle, const char *key, char *err,
                                  uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    if (kvspaceShmDeltree((kvspace_t *)handle, key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, err, err_cap, "deltree", key);
    return KVSPACE_OK;
}

KVSPACE_PUBLIC int kvspaceCpdir(void *handle, const char *source_key,
                                const char *destination_key, char *err,
                                uint32_t err_cap) {
    if (!handle || !source_key || !destination_key)
        return KVSPACE_INVALID;
    if (kvspaceShmCptree((kvspace_t *)handle, source_key, destination_key) != 0)
        return kvspace_fail(KVSPACE_BACKEND, err, err_cap, "cpdir", source_key);
    return KVSPACE_OK;
}

KVSPACE_PUBLIC int kvspaceListlen(void *handle, const char *key,
                                  int64_t *out_count,
                                  uint32_t *out_max_name_length, char *err,
                                  uint32_t err_cap) {
    if (!handle || !key || !out_count || !out_max_name_length)
        return KVSPACE_INVALID;
    *out_count = 0;
    *out_max_name_length = 0;
    int internal = kvspaceShmStorageListlen((kvspace_t *)handle, key, out_count,
                                            out_max_name_length);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "listlen",
                        key);
}

KVSPACE_PUBLIC int kvspaceList(void *handle, const char *key, char *out_names,
                               uint32_t out_capacity, uint32_t *out_length,
                               uint32_t *out_count, uint64_t *out_generation,
                               char *err, uint32_t err_cap) {
    if (!handle || !key)
        return KVSPACE_INVALID;
    int internal = kvspaceShmStorageList((kvspace_t *)handle, key, out_names,
                                         out_capacity, out_length, out_count,
                                         out_generation);
    if (internal == KVSPACE_OK)
        return KVSPACE_OK;
    return kvspace_fail(internal, err, err_cap, "list", key);
}
