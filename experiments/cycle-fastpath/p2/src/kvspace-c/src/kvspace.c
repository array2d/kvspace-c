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
#include "xvalue_head.h"
#include "xvalue_meta.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KVS_MAGIC "kvspace-c.v5"
#define ART_PREFIX_MAX 10
#define ART_NODE_MAX_SZ 2112
#define ART_SLAB_INIT (256UL * 1024 * 1024)
/* blocks_init picks 30-bit ids only if the initial pool holds > 8191 blocks;
   a smaller pool would be capped at 16384 blocks forever. */
#define SBO_HEAD_POOL_INIT (4UL * 1024 * 1024)
/* Reserved VA per region: grow in place, base never moves.
   data grows x64 per step; 2^40 fits 8 * 64^6 = 512GB. */
#define REGION_RESERVE (1ULL << 38)
#define DATA_RESERVE (1ULL << 40)
/* Values >= 64KB are level-3+ objects (32KB aligned): punch pages on delete. */
#define SBO_PUNCH_MIN (64UL * 1024)
#define WATCH_TABLE_SZ 256

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

static int art_node_sz(int t) {
    return t == ART_N4    ? sizeof(art_n4_t)
           : t == ART_N16 ? sizeof(art_n16_t)
           : t == ART_N48 ? sizeof(art_n48_t)
                          : sizeof(art_n256_t);
}

/* 进程注册表槽：pid + starttime 双重判活（pid 复用），recovery_ack = 已确认的代。 */
#define P2_SLOT_N 64
/* CLAIMING：已 CAS 占位、pid/starttime 尚未写完；扫描者视为存活，避免认领竞态误回收。 */
enum { P2_SLOT_FREE = 0, P2_SLOT_MINE = 1, P2_SLOT_RECOVERING = 2, P2_SLOT_CLAIMING = 3 };
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
    uint32_t format_version;   /* = 2（v5：art val 原子字 + 垃圾环） */
    uint32_t recovery_epoch;   /* 屏障轮次（u32，模比较） */
    uint32_t ack_seq;          /* 跨进程 futex 字（第 3 步） */
    uint32_t barrier_state;
    uint64_t barrier_pid, barrier_starttime;
    uint64_t garb_cap, garb_head, garb_tail, garb_bytes;
    uint64_t garb_freeing;
    uint64_t round_ok, round_aborted, leak_boxes;
    uint32_t claim_seq;        /* 30 位发号；耗尽 open 硬失败 */
    uint32_t _pad;
    p2_slot_t slots[P2_SLOT_N];
} kvspace_hdr_t;

#define ART_OFF (sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t))

/* blocks_init fixes the block-head width (30-bit ids) from the initial size. */
_Static_assert(ART_SLAB_INIT / (ART_NODE_MAX_SZ + 2) > 32767ULL / 4,
               "ART_SLAB_INIT too small: blockmalloc would pick 14-bit ids");
_Static_assert(REGION_RESERVE / (ART_NODE_MAX_SZ + 4) <= (1ULL << 30),
               "REGION_RESERVE too large for 30-bit block ids");
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

typedef struct {
    char key[256];
    pthread_cond_t cond;
    pthread_mutex_t mtx;
    uint8_t *val;
    int32_t val_len;
    bool ready;
} watch_t;

#define P2_TOMB_MAX 256

struct kvspace {
    shm_region_t r_art, r_head, r_data, r_gen, r_garb;
    garb_ent_t *garb;             /* .garb 映射（GARB_CAP 条） */
    uint64_t gen_n;               /* gens_at 容量：本进程已映射的节点数 */
    uint64_t art_total_seen;      /* 上次同步时的 ART slab total_size */
    kvspace_hdr_t *hdr;
    uint64_t store_id;   /* 进程内句柄身份（P2 token 用） */
    int p2_slot;         /* 注册表槽；-1 = 未注册 */
    uint32_t p2_epoch;   /* 本进程已 ack 的 recovery_epoch */
    blocks_meta_t *art_meta;
    uint8_t *art_data;
    sbo_meta_t *sbo_meta;
    uint8_t *sbo_data;
    watch_t watches[WATCH_TABLE_SZ];
    pthread_mutex_t wlock;
};

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

static void region_close(shm_region_t *r); /* fwd */

static void region_close(shm_region_t *r) {
    if (r->base)
        munmap(r->base, r->reserve);
    if (r->fd >= 0)
        close(r->fd);
}

/* Pick up growth done by another process (mid-op growth: kvspace#11). */

/* ── P2: 旁路 gen 表（node id → {struct_gen, value_gen}）──────────── */
static void kv_sbo_free(kvspace_t *kv, uint64_t off);
static void p2_setstats_dump(void);
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id);
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b);
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl);
static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard);
static int32_t art_follow(kvspace_t *kv, int32_t id);
static int32_t ref_leaf(kvspace_t *kv, kvspaceRef_t *ref, const char *key);

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
            (unsigned long long)t, (unsigned long long)kv->hdr->garb_bytes,
            (unsigned long long)h, GE_STATE(m), GE_CLAIM(m));
}

static int garb_reserve(kvspace_t *kv, uint64_t *idx) {
    uint32_t claim = kv->p2_slot >= 0 ? kv->hdr->slots[kv->p2_slot].claim_id : 0;
    if (!claim)
        return -1;
    for (;;) {
        uint64_t h = __atomic_load_n(&kv->hdr->garb_head, __ATOMIC_ACQUIRE);
        uint64_t t = __atomic_load_n(&kv->hdr->garb_tail, __ATOMIC_RELAXED);
        if (t - h >= kv->hdr->garb_cap) {
            garb_full_report(kv);
            return -1;
        }
        if (__atomic_compare_exchange_n(&kv->hdr->garb_tail, &t, t + 1, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            garb_ent_t *e = &kv->garb[t & (GARB_CAP - 1)];
            __atomic_store_n(&e->meta, GE_MAKE(GE_RESERVED, claim),
                             __ATOMIC_RELEASE);
            *idx = t;
            return 0;
        }
    }
}

static void garb_commit(kvspace_t *kv, uint64_t idx, uint64_t off) {
    garb_ent_t *e = &kv->garb[idx & (GARB_CAP - 1)];
    uint32_t claim = GE_CLAIM(__atomic_load_n(&e->meta, __ATOMIC_ACQUIRE));
    e->off = off;
    e->epoch = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&e->meta, GE_MAKE(GE_COMMITTED, claim), __ATOMIC_RELEASE);
    __atomic_add_fetch(&kv->hdr->garb_bytes,
                       sbo_allocated_size(kv->sbo_meta, off), __ATOMIC_ACQ_REL);
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
        if (!h || h->type == ART_MOVED) {
            if (getenv("P2_DEBUG")) { fprintf(stderr, "DBG node_at FAIL key=%s nid=%d type=%d d=%d/%d\n", key, nid, h ? (int)h->type : -1, d, klen); }
            return -1;
        }
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len,
                               (const uint8_t *)key + d, klen - d);
            if (s != h->prefix_len && d + s != klen) {
                if (getenv("P2_DEBUG")) { fprintf(stderr, "DBG node_at PFX key=%s nid=%d d=%d s=%d plen=%d klen=%d\n", key, nid, d, s, (int)h->prefix_len, klen); }
                return -1;
            }
            d += s;
        }
        /* 键可能正好结束在 prefix_len==0 的分支节点上（如 k1 落在根 N4 的子叶），
         * 此时 d==klen，必须在此返回；否则会越界读 key[klen]。 */
        if (d == klen)
            return nid;
        nid = art_child(kv, h, (uint8_t)key[d]);
        d++;
    }
    if (getenv("P2_DEBUG")) { fprintf(stderr, "DBG node_at EOF key=%s d=%d/%d\n", key, d, klen); }
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
        if (v == P2_SLOT_RECOVERING) {
            /* 上一轮屏障超时遗留：死者回收，活者回到 MINE 重新等待，避免槽永久卡死。 */
            uint32_t to = p2_slot_alive(s) ? P2_SLOT_MINE : P2_SLOT_FREE;
            __atomic_compare_exchange_n(&s->state, &v, to, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
            continue;
        }
        if (v != P2_SLOT_FREE && !p2_slot_alive(s))
            __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    if (getenv("P2_DEBUG")) {
        int nf = 0, nm = 0;
        for (int i = 0; i < P2_SLOT_N; i++) {
            uint32_t v = __atomic_load_n(&kv->hdr->slots[i].state, __ATOMIC_ACQUIRE);
            if (v == P2_SLOT_FREE) nf++;
            else if (v == P2_SLOT_MINE) nm++;
        }
        fprintf(stderr, "DBG claim pid=%u start=%llu free=%d mine=%d\n", me,
                (unsigned long long)st, nf, nm);
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
        s->quiesced = 0;
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
    p2_slot_t *s = &kv->hdr->slots[kv->p2_slot];
    uint32_t expect = P2_SLOT_MINE;
    __atomic_compare_exchange_n(&s->state, &expect, P2_SLOT_FREE, false,
                                __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    kv->p2_slot = -1;
}

/* 每次 kvspaceView* 入口：屏障代变化 → 记本地代并 ack（token 由 _pad 校验失效）。 */
static void p2_reg_ack(kvspace_t *kv) {
    uint32_t ep = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    if (kv->p2_epoch == ep)
        return;
    kv->p2_epoch = ep;
    if (kv->p2_slot >= 0)
        __atomic_store_n(&kv->hdr->slots[kv->p2_slot].recovery_ack, ep,
                         __ATOMIC_RELEASE);
}

/* 表随 ART slab 增长（容量按当前 block 配置重算）。 */
#define GEN_RESERVE (64ULL << 20)   /* 64MiB VA 预留：8M 节点 */
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


/* ── P2: tombstone 延迟释放（粗粒度 epoch：队满或显式 flush 时统一 free）── */


int kvspaceShmSlotView(kvspace_t *kv, const kvspaceRef_t *ref,
                       const char *key_fallback, kvspaceSlotView_t *out);
static int32_t art_id_of(kvspace_t *kv, const void *p);
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id);
static int32_t art_follow(kvspace_t *kv, int32_t id);
static int32_t ref_leaf(kvspace_t *kv, kvspaceRef_t *ref, const char *key);
static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard);
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b);
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl);

/* ── P2: 版本化 view token ─────────────────────────────────────── */
int kvspaceShmViewFromRef(kvspace_t *kv, const kvspaceRef_t *ref,
                           const char *key_fallback, kvspaceViewToken_t *tok) {
    (void)key_fallback;
    if (!kv || !ref || !tok || ref->gen != 0)
        return -1;
    p2_reg_ack(kv);
    int32_t id = art_follow(kv, (int32_t)ref->block_id);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (!h)
        return -1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return -1;
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(hv));
    if (cap < 1)
        return -1;
    uint8_t *raw = kv->sbo_data + AV_OFF(hv);
    kvspaceXh w;
    if (kvspaceXhHeadView(raw, cap, &w) != 0 || w.total > cap ||
        w.kind == KVSPACE_XH_EXT || (w.kind & KVSPACE_XH_PTR_FLAG))
        return -1;
    kv_gens_t *g = gens_at(kv, id);
    if (!g)
        return -1;
    uint32_t gen = __atomic_load_n(&g->struct_gen, __ATOMIC_ACQUIRE);
    tok->head = raw;
    tok->body = raw + w.headlen;
    tok->headlen = w.headlen;
    tok->value_len = (uint32_t)w.content_len;
    tok->cap = cap;
    tok->body_cap = cap - w.headlen;
    tok->struct_gen = gen;
    tok->value_gen = __atomic_load_n(&g->value_gen, __ATOMIC_ACQUIRE);
    tok->store_id = kv->store_id;
    tok->node_id = (uint32_t)id;
    tok->_pad = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);
    return 0;
}

int kvspaceShmViewResolve(kvspace_t *kv, const char *key, kvspaceViewToken_t *tok) {
    kvspaceRef_t ref;
    if (!kv || !key || !tok || kvspaceShmResolveRef(kv, key, &ref) != 0)
        return -1;
    return kvspaceShmViewFromRef(kv, &ref, key, tok);
}

/* 前缀解析：path 的最后一个字节落在哪个节点；depth = 该节点压缩前缀被 path 覆盖的字节数
 * （可能 < prefix_len，即边界落在前缀内部）。用于 short 键（整键就是一个节点的前缀）。 */
int kvspaceShmResolvePrefix(kvspace_t *kv, const char *path, kvspaceRef_t *ref) {
    if (!kv || !path || !ref)
        return -1;
    memset(ref, 0, sizeof(*ref));
    int klen = (int)strlen(path);
    int d = 0;
    int32_t nid = kv->hdr->art_root;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len,
                               (const uint8_t *)path + d, klen - d);
            if (s != h->prefix_len && d + s != klen)
                return -1;
            d += s;
            if (d == klen) {
                ref->block_id = (uint32_t)nid;
                ref->depth = (uint32_t)s;   /* 该节点前缀里被 path 覆盖的字节数 */
                return 0;
            }
        }
        nid = art_child(kv, h, (uint8_t)path[d]);
        d++;
    }
    return -1;
}

/* 前缀形态：parent_ref 的 block_id/depth 指向目录节点；只走 rel。 */
int kvspaceShmViewResolveFrom(kvspace_t *kv, const kvspaceRef_t *parent,
                              const char *rel, kvspaceViewToken_t *tok) {
    if (!kv || !parent || !rel || !tok)
        return -1;
    int32_t pid = art_follow(kv, (int32_t)parent->block_id);
    art_hdr_t *ph = pid >= 0 ? art_hdr(kv, pid) : NULL;
    if (!ph)
        return -1;
    /* 边界落在节点压缩前缀内部时，先把该节点前缀的剩余字节与 rel 对掉。 */
    const uint8_t *p = (const uint8_t *)rel;
    int rem = (int)strlen(rel);
    int32_t covered = (int32_t)parent->depth;
    while (covered < (int32_t)ph->prefix_len && rem > 0) {
        if (p[0] != ph->prefix[covered])
            return -1;
        p++; rem--; covered++;
    }
    int32_t id;
    if (rem == 0) {
        id = pid;                        /* 空 rel = 前缀自身 */
    } else {
        int32_t cid = art_child(kv, ph, p[0]);
        id = art_walk(kv, cid, p, rem, 1, NULL, NULL);
    }
    if (id < 0)
        return -1;
    kvspaceRef_t leaf = {0};
    leaf.block_id = (uint32_t)id;
    leaf.gen = 0;
    return kvspaceShmViewFromRef(kv, &leaf, rel, tok);
}

int kvspaceShmViewCheck(kvspace_t *kv, const kvspaceViewToken_t *tok) {
    if (!kv || !tok)
        return -1;
    p2_reg_ack(kv);
    if (tok->store_id != kv->store_id)
        return 1;
    if (tok->_pad != __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE))
        return 1;
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    if (!g)
        return 1;
    if (__atomic_load_n(&g->struct_gen, __ATOMIC_ACQUIRE) != tok->struct_gen)
        return 1;
    art_hdr_t *h = art_hdr(kv, (int32_t)tok->node_id);
    if (!h)
        return 1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 1;
    if (tok->head != kv->sbo_data + AV_OFF(hv))
        return 1;
    return 0;
}

/* v2.2 无 pin：token 是调用方持有的普通数据，Release 只做本地作废。 */
int kvspaceShmViewRelease(kvspace_t *kv, kvspaceViewToken_t *tok) {
    if (!kv || !tok)
        return -1;
    if (tok->store_id != kv->store_id)
        return 1;
    tok->store_id = 0;
    return 0;
}

/* 热路径写：同尺寸就地写（8B inline 情形）。0=成功；1=需要回落（尺寸/版本不符）。 */
int kvspaceShmViewWrite(kvspace_t *kv, kvspaceViewToken_t *tok,
                        const uint8_t *data, uint32_t len) {
    if (kvspaceShmViewCheck(kv, tok) != 0)
        return 1;
    if (len > tok->body_cap)
        return 1;
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    if (!g)
        return 1;
    __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL); /* 奇：写中 */
    memcpy(tok->body, data, len);
    __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL); /* 偶：写完 */
    tok->value_len = len;
    return 0;
}

/* 热路径删：tombstone + value_gen++（box 释放走延迟队列）。 */
int kvspaceShmViewDel(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key) {
    if (kvspaceShmViewCheck(kv, tok) != 0)
        return 1;
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    art_hdr_t *h = art_hdr(kv, (int32_t)tok->node_id);
    if (!g || !h)
        return 1;
    /* 叶子级逻辑墓碑：CAS 一次原子清零 has_value+box；不 bump struct_gen。 */
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 1;
    uint64_t gidx;
    if (garb_reserve(kv, &gidx) != 0)
        return -1;                      /* 表满：硬错误，未变更 */
    if (!art_val_cas(h, &hv, 0)) {
        garb_cancel(kv, gidx);
        return 1;
    }
    __atomic_add_fetch(&g->value_gen, 2u, __ATOMIC_ACQ_REL); /* 静止态保持偶数 */
    garb_commit(kv, gidx, AV_OFF(hv));
    (void)key;
    return 0;
}

/* 恢复屏障：检测死 owner → 推进 recovery_epoch → 等存活进程 ack → 回收死槽。
 * 返回 1 = 屏障完成（无死进程或全部存活进程已 ack）；0 = 超时（不回收，记审计）。 */
static int p2_barrier(kvspace_t *kv) {
#if !defined(__linux__)
    return 1;                        /* 无 /proc：不判死，不做墓碑回收 */
#else
    /* 先修复上一轮超时遗留的 RECOVERING：活者回 MINE 重新等待，死者回 FREE。 */
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
        if (v == P2_SLOT_RECOVERING) {
            uint32_t to = p2_slot_alive(s) ? P2_SLOT_MINE : P2_SLOT_FREE;
            __atomic_compare_exchange_n(&s->state, &v, to, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
        }
    }
    /* 单趟扫：收集死槽下标（/proc 读是主要成本，避免两趟）。 */
    uint8_t dead_idx[P2_SLOT_N];
    int dead = 0;
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) == P2_SLOT_MINE &&
            !p2_slot_alive(s))
            dead_idx[dead++] = (uint8_t)i;
    }
    if (!dead)
        return 1;
    uint32_t ns = __atomic_add_fetch(&kv->hdr->recovery_epoch, 1u, __ATOMIC_ACQ_REL);
    for (int k = 0; k < dead; k++) {
        p2_slot_t *s = &kv->hdr->slots[dead_idx[k]];
        uint32_t v = P2_SLOT_MINE;
        __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_RECOVERING, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    p2_reg_ack(kv);                 /* sweeper 自己也确认新代 */
    int ms = getenv("P2_BARRIER_MS") ? atoi(getenv("P2_BARRIER_MS")) : 200;
    bool done = false;
    /* 先短自旋（sched_yield），再退到 50µs 轮询：正常 ack 在微秒级，避免 1ms 量化。 */
    long rounds = (long)ms * 20;
    for (long t = 0; t < rounds; t++) {
        done = true;
        for (int i = 0; i < P2_SLOT_N; i++) {
            p2_slot_t *s = &kv->hdr->slots[i];
            if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != P2_SLOT_MINE)
                continue;
            if (__atomic_load_n(&s->recovery_ack, __ATOMIC_ACQUIRE) != ns) {
                done = false;
                break;
            }
        }
        if (done)
            break;
        if (t < 4096)
            sched_yield();
        else
            usleep(50);
    }
    if (!done) {
        fprintf(stderr,
                "kvspace: recovery barrier timeout (epoch %u, waited %dms); "
                "dead pins not reclaimed\n", ns, ms);
        if (getenv("P2_DEBUG")) {
            for (int i = 0; i < P2_SLOT_N; i++) {
                p2_slot_t *s = &kv->hdr->slots[i];
                uint32_t st = __atomic_load_n(&s->state, __ATOMIC_ACQUIRE);
                if (st == P2_SLOT_MINE)
                    fprintf(stderr, "DBG slot %d pid=%u ack=%llu want=%u\n", i,
                            s->pid, (unsigned long long)s->recovery_ack, ns);
            }
        }
        return 0;
    }
    for (int i = 0; i < P2_SLOT_N; i++) {
        p2_slot_t *s = &kv->hdr->slots[i];
        uint32_t v = P2_SLOT_RECOVERING;
        __atomic_compare_exchange_n(&s->state, &v, P2_SLOT_FREE, false,
                                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    }
    return 1;
#endif
}

/* 第 2 步：回收由垃圾环 + 屏障轮负责（drain 在第 3 步接）。ViewSweep = 强制一轮屏障。 */
int kvspaceShmViewSweep(kvspace_t *kv) {
    if (!kv || kv->hdr->art_root < 0)
        return 0;
    p2_reg_ack(kv);
    p2_barrier(kv);                 /* 死 owner 的代推进 + 槽回收 */
    return 0;
}

int kvspaceShmViewFlushTomb(kvspace_t *kv) {
    return kvspaceShmViewSweep(kv);   /* 语义并入屏障轮（drain 在第 3 步接） */
}

int kvspaceShmViewRefresh(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key) {
    kvspaceViewToken_t t;
    if (kvspaceShmViewResolve(kv, key, &t) != 0)
        return -1;
    *tok = t;
    return 0;
}

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
static bool sbo_pool_full(sbo_meta_t *m) {
    blocks_meta_t *p = sbo_pool(m);
    if (p->free_next_id != -1)
        return false;
    uint64_t next_end = (uint64_t)block_offset(p, p->malloc_blocks) +
                        p->sizeof_block_head + p->block_size;
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

/* P2: tombstone —— 有界 ring（L1 内）+ 后台 flush 线程。
 * 前台只做：ring[tail & MASK] = off; tail++（release）; 唤醒后台。
 * 队列满时前台等待（backpressure），绝不在调用线程里 sbo_free。 */
/* ---- helpers ---- */
static void *art_blk(kvspace_t *kv, int32_t id) {
    if (id < 0)
        return NULL;
    return kv->art_data + blockdata_offset(kv->art_meta, (uint64_t)id);
}
static int32_t art_balloc(kvspace_t *kv) {
    int64_t id = blocks_alloc(kv->art_meta, kv->art_data);
    if (id < 0) {
        if (art_slab_grow(kv) != 0 || gens_grow(kv) != 0)
            return -1;
        id = blocks_alloc(kv->art_meta, kv->art_data);
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
            int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
            if (s != h->prefix_len) {
                if (d + s < klen)
                    return -1;
                if (s < h->prefix_len)
                    return -1;
            }
            d += h->prefix_len;
            if (d > klen)
                return -1;
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
    int32_t id = art_balloc(kv);
    if (id < 0)
        return -1;
    art_n4_t *x = art_blk(kv, id);
    memset(x, 0, sizeof(*x));
    x->h.type = ART_N4;
    art_val_store(&x->h, AV_MAKE(1, off));
    return id;
}
static int32_t art_new_node(kvspace_t *kv, int t) {
    int32_t id = art_balloc(kv);
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
        (void)gens_at;
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
        int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
        if (s != h->prefix_len || d + h->prefix_len > klen)
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
        garb_commit(kv, gidx, AV_OFF(hv));
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
        art_slab = (size_t)tmp.art_slab_size;
        sbo_head = (size_t)tmp.sbo_head_size;
        data_size = (size_t)tmp.sbo_data_size;
    }

    /* O_TRUNC: sbo_init rejects a stale magic */
    int fl = created ? O_RDWR | O_CREAT | O_TRUNC : O_RDWR;
    if (region_attach(&kv->r_art, created, ART_OFF + art_slab,
                      REGION_RESERVE) != 0 ||
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
        kv->hdr->format_version = 2;
        kv->hdr->garb_cap = GARB_CAP;
        kv->hdr->art_slab_size = art_slab;
        kv->hdr->sbo_head_size = sbo_head;
        kv->hdr->sbo_data_size = data_size;
        kv->hdr->art_root = -1;
        if (blocks_init(kv->art_meta, art_slab, ART_NODE_MAX_SZ) != 0 ||
            sbo_init(kv->sbo_meta, sbo_head, data_size) != 0)
            goto fail;
        /* magic last: half-initialized file fails reopen */
        memcpy(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1);
    }
    if (!created &&
        (kv->hdr->format_version != 2 || kv->hdr->garb_cap != GARB_CAP)) {
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
    kv->p2_epoch = __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE);

    pthread_mutex_init(&kv->wlock, NULL);
    for (int i = 0; i < WATCH_TABLE_SZ; i++) {
        pthread_cond_init(&kv->watches[i].cond, NULL);
        pthread_mutex_init(&kv->watches[i].mtx, NULL);
    }
    return kv;

fail:
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
void kvspaceShmClose(kvspace_t *kv) {
    if (!kv)
        return;
    p2_setstats_dump();
    p2_reg_release(kv);
    for (int i = 0; i < WATCH_TABLE_SZ; i++) {
        pthread_cond_destroy(&kv->watches[i].cond);
        pthread_mutex_destroy(&kv->watches[i].mtx);
        free(kv->watches[i].val);
    }
    pthread_mutex_destroy(&kv->wlock);
    region_close(&kv->r_art);
    region_close(&kv->r_head);
    region_close(&kv->r_data);
    region_close(&kv->r_gen);
    region_close(&kv->r_garb);
    free(kv);
}

/* ---- link resolve helpers ---- */
static int read_value(kvspace_t *kv, uint64_t off, uint8_t **out, int32_t *ol) {
    uint8_t *s = kv->sbo_data + off;
    size_t sz = sbo_allocated_size(kv->sbo_meta, off);
    kvspaceXh wire;
    if (kvspaceXhView(s, sz, &wire) == 0) {
        if (wire.total > INT32_MAX)
            return -1;
        *out = s;
        /* 空 langtype 只在 Ptr/Ext 上是合法值（“目标不约束”）；其余空 langtype = None。 */
        bool ref = wire.kind == KVSPACE_XH_EXT ||
                   wire.kind == (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG);
        *ol = (wire.langtype_len || ref) ? (int32_t)wire.total : 0;
        return 0;
    }
    return -1;
}
/* link 解析：ptr（ref=1）与 @ext（ref=2）的 XValue 恒为叶子（spec 硬规则——指针/扩展键
 * 之后不可能再有成员，成员只挂在 target 上），故路径中任何 `/` 分隔的容器前缀都绝不会是
 * link；唯一可能是 ptr 的只有整键叶子本身。因此只需查整键一次：非 ptr 即完成，ptr 则顺链
 * 到 target（本身又是一个完整叶子键）再查，最多追 16 跳。叶子不存在→非链，原样返回（调用
 * 方另做 extindex/dir 兜底）。取代旧的「逐前缀 1~3 次 art_search + 追加 rest」全下降探测。 */
static void resolve_path(kvspace_t *kv, const char *path, char *out, int osz) {
    strncpy(out, path, osz - 1);
    out[osz - 1] = '\0';
    for (int depth = 0; depth < 16; depth++) {
        art_hdr_t *h =
            art_search(kv, kv->hdr->art_root, (const uint8_t *)out, (int)strlen(out));
        if (!h)
            return;
        uint64_t hv = art_val_load(h);
        if (!AV_HAS(hv))
            return;
        uint8_t *raw;
        int32_t rl;
        if (read_value(kv, AV_OFF(hv), &raw, &rl) < 0)
            return;
        kvspaceXh wire;
        if (rl > 0 && kvspaceXhView(raw, (uint64_t)rl, &wire) == 0) {
            if (wire.kind != (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG) ||
                wire.content_len >= (uint64_t)osz)
                return;
            memcpy(out, wire.body, (size_t)wire.content_len);
            out[wire.content_len] = 0;
            continue;
        }
        return;
    }
}

/* resolve_path 的融合版：解析 link 链的同时把终端叶子的值一并取出，令调用方省掉
 * 「解析后再 art_search + read_value」的整套重复下降。每跳只解一次 head（ref 判定与
 * 值长度共用），命中带值节点即返 fetched=1（*raw/*rl 就位）。语义与
 * resolve_path + art_search + read_value 逐字等价：未命中/无值 → 返回该节点、fetched=0，
 * 交调用方走 extindex/dir 兜底。 */
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
        size_t sz = sbo_allocated_size(kv->sbo_meta, AV_OFF(hv));
        kvspaceXh wire;
        if (kvspaceXhView(s, sz, &wire) == 0) {
            if (wire.total > INT32_MAX)
                return NULL;
            if (wire.kind != (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG) ||
                wire.content_len >= (uint64_t)osz) {
                *raw = s;
                bool ref = wire.kind == KVSPACE_XH_EXT ||
                           wire.kind == (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG);
                *rl = (wire.langtype_len || ref) ? (int32_t)wire.total : 0;
                *fetched = 1;
                return h;
            }
            memcpy(out, wire.body, (size_t)wire.content_len);
            out[wire.content_len] = 0;
            continue;
        }
        return NULL;
    }
    return NULL;
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
    if (read_value(kv, AV_OFF(hv), &raw, &rl) < 0)
        return 0;
    kvspaceXh wire;
    if (rl <= 0 || kvspaceXhDecode(raw, (uint64_t)rl, &wire) != 0 ||
        wire.kind != KVSPACE_XH_EXT || wire.a == 0 || wire.a >= (uint64_t)osz)
        return 0;
    memcpy(out, wire.body, (size_t)wire.a);
    out[wire.a] = 0;
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
                if (read_value(kv, AV_OFF(ehv), &raw, &rl) == 0) {
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
    if (!fetched && read_value(kv, AV_OFF(hv), &raw, &rl) < 0)
        return NULL;
    *ol = rl;
    return raw;
}

/* gen>0: block_id is parent at depth gen; key must share that prefix. */
static int32_t ref_leaf(kvspace_t *kv, kvspaceRef_t *ref, const char *key) {
    int32_t id = art_follow(kv, (int32_t)ref->block_id);
    if (id < 0)
        return -1;
    ref->block_id = (uint32_t)id;
    if (ref->gen == 0)
        return id;
    if (!key)
        return -1;
    int klen = (int)strlen(key);
    int d = (int)ref->gen;
    if (d < 0 || d > klen)   /* P2: d == klen 表示前缀节点就是全部路径 */
        return -1;
    if (d == klen)
        return art_follow(kv, (int32_t)ref->block_id);
    art_hdr_t *h = art_hdr(kv, id);
    if (!h || h->type == ART_MOVED)
        return -1;
    int32_t cid = art_child(kv, h, (uint8_t)key[d]);
    if (cid < 0)
        return -1;
    return art_walk(kv, cid, (const uint8_t *)key, klen, d + 1, NULL, NULL);
}

int kvspaceShmResolveRef(kvspace_t *kv, const char *key, kvspaceRef_t *ref) {
    if (!kv || !key || !ref || reserved_meta_path(key))
        return -1;
    memset(ref, 0, sizeof(*ref));
    if (kv_sync(kv) != 0)
        return -1;
    int klen = (int)strlen(key);
    int seplen = 0;
    int sep = last_key_sep((const uint8_t *)key, klen, &seplen);
    int dir_d = 0;
    int32_t dn = -1;
    int32_t id = art_find_dir(kv, kv->hdr->art_root, (const uint8_t *)key, klen,
                              sep, seplen, &dn, &dir_d);
    if (id < 0)
        return -1;
    ref->block_id = (uint32_t)id;
    ref->gen = 0;
    if (dn >= 0 && dir_d > 0) {
        ref->parent_id = (uint32_t)dn;
        ref->depth = (uint32_t)dir_d;
    } else {
        ref->parent_id = 0;
        ref->depth = 0;
    }
    return 0;
}

uint8_t *kvspaceShmGetByRef(kvspace_t *kv, kvspaceRef_t *ref,
                            const char *key_fallback, int32_t *ol) {
    if (!kv || !ref || !ol)
        return NULL;
    *ol = 0;
    if (kv_sync(kv) != 0)
        return NULL;
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    uint64_t hv = h ? art_val_load(h) : 0;
    if (h && AV_HAS(hv)) {
        uint8_t *raw;
        int32_t rl;
        if (read_value(kv, AV_OFF(hv), &raw, &rl) == 0) {
            *ol = rl;
            return raw;
        }
    }
    /* gen>0: parent walk miss — do not full-Get; caller falls back. */
    if (ref->gen != 0)
        return NULL;
    if (!key_fallback)
        return NULL;
    uint8_t *raw = kvspaceShmGet(kv, key_fallback, 0, ol);
    if (raw)
        kvspaceShmResolveRef(kv, key_fallback, ref);
    return raw;
}

int kvspaceShmSetPartByRef(kvspace_t *kv, kvspaceRef_t *ref,
                           const char *key_fallback, uint32_t offset,
                           const uint8_t *buf, uint32_t buf_len) {
    if (!kv || !ref || !buf)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    /* SLACK、pow=6（headlen=64）的 part-write 快路径：跳过 xh_view 结构解码，
     * 只做必要的 head 自洽 + per-box 物理容量校验后写正文。任何不自洽
     * （gen>0、无值、head 不足 64B、pow/kind/Ptr 不符、langtype 无 NUL、
     * a>b、b/offset/buf_len 越界、超出 box 物理容量）一律回退现有路径，
     * 行为不变。不修改 head，不更新 a/b。 */
    uint64_t hv = h ? art_val_load(h) : 0;
    if (h && AV_HAS(hv) && ref->gen == 0) {
        uint64_t data_size = kv->hdr->sbo_data_size;
        uint64_t off = AV_OFF(hv);
        if (off <= data_size && data_size - off >= 64) {
            const uint8_t *hs = kv->sbo_data + off;
            uint8_t flags = hs[1];
            if (hs[0] == KVSPACE_XH_POW_SLACK &&
                (flags & 3u) == KVSPACE_XH_SLACK &&
                (flags & KVSPACE_XH_PTR_FLAG) == 0) {
                /* langtype 必须在 64B head 内以 NUL 终止（不能只看首字节非空）。 */
                bool lt_ok = false;
                for (uint32_t i = KVSPACE_XH_PREFIX; i < 64; i++)
                    if (hs[i] == 0) {
                        lt_ok = i > KVSPACE_XH_PREFIX; /* 非空 */
                        break;
                    }
                uint64_t a = 0, b = 0;
                for (int i = 0; lt_ok && i < 8; i++) {
                    a |= (uint64_t)hs[2 + i] << (8 * i);
                    b |= (uint64_t)hs[10 + i] << (8 * i);
                }
                if (lt_ok && a <= b) {
                    /* per-box 物理容量：损坏 head 的 b 可能大于 box 但仍在全局内，
                     * 不做这一步会跨 box 写坏相邻值。 */
                    uint64_t cap = sbo_allocated_size(kv->sbo_meta, off);
                    if (cap >= 64 && cap <= data_size - off && b <= cap - 64 &&
                        (uint64_t)offset <= 64 + b &&
                        (uint64_t)buf_len <= 64 + b - (uint64_t)offset) {
                        memcpy(kv->sbo_data + off + offset, buf, buf_len);
                        return 0;
                    }
                }
            }
        }
    }
    if (!h || !AV_HAS(hv)) {
        if (ref->gen != 0)
            return -1;
        if (!key_fallback)
            return -1;
        int32_t rl = 0;
        uint8_t *d = kvspaceShmGet(kv, key_fallback, 0, &rl);
        if (!d || offset + buf_len > (uint32_t)rl)
            return -1;
        memcpy(d + offset, buf, buf_len);
        kvspaceShmResolveRef(kv, key_fallback, ref);
        return 0;
    }
    uint8_t *raw;
    int32_t rl;
    if (read_value(kv, AV_OFF(hv), &raw, &rl) < 0 ||
        offset + buf_len > (uint32_t)rl)
        return -1;
    memcpy(raw + offset, buf, buf_len);
    return 0;
}

/* /vthread/<vid>/‥status：合法内容长度上限与创建时物理容量下限。 */
#define KVSPACE_STATUS_MAX_LEN 16u
#define KVSPACE_STATUS_MIN_CAP 80u

/* status 的规范形状：SLACK、headlen=64、langtype "[<digits>]char/utf8"。 */
static int is_status_langtype(const kvspaceXh *w) {
    if (!w->langtype || w->langtype_len < 12)
        return 0;
    const uint8_t *p = w->langtype;
    uint32_t n = w->langtype_len, i = 0;
    if (p[i++] != '[')
        return 0;
    uint32_t d0 = i;
    while (i < n && p[i] >= '0' && p[i] <= '9')
        i++;
    if (i == d0 || i >= n || p[i++] != ']')
        return 0;
    static const char suf[] = "char/utf8";
    return n - i == sizeof suf - 1 && memcmp(p + i, suf, sizeof suf - 1) == 0;
}

static int is_vthread_status_key(const char *key) {
    static const char pfx[] = "/vthread/";
    static const char suf[] = "/\xE2\x80\xA5status";
    if (!key)
        return 0;
    size_t n = strlen(key), sl = sizeof suf - 1;
    return n > sl && strncmp(key, pfx, sizeof pfx - 1) == 0 &&
           memcmp(key + n - sl, suf, sl) == 0;
}

int kvspaceShmSlotView(kvspace_t *kv, const kvspaceRef_t *ref,
                       const char *key_fallback, kvspaceSlotView_t *out) {
    /* status 识别依赖 key_fallback；view 一律要求非 NULL（runtime 必须传）。 */
    if (!kv || !ref || !out || !key_fallback)
        return -1;
    if (ref->gen != 0)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    kvspaceRef_t r = *ref;
    int32_t id = ref_leaf(kv, &r, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (!h)
        return -1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return -1;
    uint8_t *raw = NULL;
    int32_t rl = 0;
    if (read_value(kv, AV_OFF(hv), &raw, &rl) < 0 || !raw)
        return -1;
    kvspaceXh wire;
    if (kvspaceXhDecode(raw, (uint64_t)rl, &wire) != 0)
        return -1;
    /* ptr/ext 槽不可视：没有稳定的 head/body 语义。 */
    if (wire.kind == KVSPACE_XH_EXT ||
        (wire.kind & KVSPACE_XH_PTR_FLAG))
        return -1;
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(hv));
    if (cap < wire.headlen || wire.content_len > cap - wire.headlen)
        return -1;
    /* status：只暴露规范形状且物理容量达标的槽；旧小 cap/异型 → -1，
     * runtime 回退（旧 store 不自动升级）。 */
    if (is_vthread_status_key(key_fallback)) {
        if (wire.kind != KVSPACE_XH_SLACK ||
            wire.headlen != (1u << KVSPACE_XH_POW_SLACK) ||
            !is_status_langtype(&wire) || cap < KVSPACE_STATUS_MIN_CAP ||
            wire.content_len > KVSPACE_STATUS_MAX_LEN)
            return -1;
    }
    out->head = raw;
    out->headlen = wire.headlen;
    out->body = raw + wire.headlen;
    out->cap = cap;
    out->body_cap = cap - wire.headlen;
    return 0;
}

/* 临时探针（P2_SETSTATS=<file>）：统计写入形态，用于静默屏障可行性评估。 */
static struct { uint64_t same, realloc_, fresh, retired_bytes; } g_setstats;
static void p2_setstats_dump(void) {
    const char *f = getenv("P2_SETSTATS");
    if (!f || !f[0])
        return;
    FILE *fp = fopen(f, "a");
    if (!fp)
        return;
    fprintf(fp, "pid=%d same=%llu realloc=%llu fresh=%llu retired_bytes=%llu\n",
            (int)getpid(), (unsigned long long)g_setstats.same,
            (unsigned long long)g_setstats.realloc_,
            (unsigned long long)g_setstats.fresh,
            (unsigned long long)g_setstats.retired_bytes);
    fclose(fp);
}

/* 原始落盘（不处理容器/member 语义，供内部调用，避免递归）。 */
static int shm_set_local(kvspace_t *kv, const char *key, const uint8_t *val,
                         int32_t val_len, art_hdr_t *old) {
    {
        static bool reg;
        if (!reg) { reg = true; atexit(p2_setstats_dump); }
    }
    uint64_t ohv = old ? art_val_load(old) : 0;
    bool retiring = old && AV_HAS(ohv);
    uint64_t gidx = 0;
    if (retiring) {
        uint64_t cap = sbo_allocated_size(kv->sbo_meta, AV_OFF(ohv));
        if ((uint64_t)val_len <= cap) {
            /* 同尺寸就地写：指针不变，value_gen 奇偶保护（一次查表） */
            kv_gens_t *g = gens_at(kv, art_id_of(kv, old));
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            memcpy(kv->sbo_data + AV_OFF(ohv), val, (size_t)val_len);
            if (g)
                __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
            g_setstats.same++;
            return 0;
        }
        /* 换 box：reserve 在任何变更之前；失败 = 硬错误、旧值原样 */
        if (garb_reserve(kv, &gidx) != 0)
            return -1;
        g_setstats.realloc_++;
        g_setstats.retired_bytes += cap;
    } else {
        g_setstats.fresh++;
    }
    size_t alloc_len = (size_t)val_len;
    if (is_vthread_status_key(key) && alloc_len < KVSPACE_STATUS_MIN_CAP)
        alloc_len = KVSPACE_STATUS_MIN_CAP;
    uint64_t off = kv_sbo_alloc(kv, alloc_len);
    if (off == (uint64_t)-1) {
        if (retiring)
            garb_cancel(kv, gidx);
        return -1;
    }
    memcpy(kv->sbo_data + off, val, val_len);
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
        if (getenv("P2_DEBUG")) { fprintf(stderr, "DBG reset-lookup key=%s nid=%d g=%p root=%d\n", key, nid, (void *)g, kv->hdr->art_root); }
        if (g) {
            gen_bump_struct(kv, nid);
            gen_bump_value(kv, nid);
        }
    }
    if (retiring)
        garb_commit(kv, gidx, AV_OFF(ohv));
    return 0;
}

static int shm_set_raw(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len) {
    art_hdr_t *old = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                                (int)strlen(key));
    return shm_set_local(kv, key, val, val_len, old);
}

/* 键上限 1024 → meta key 最长 15 + 2*1023 + 1。 */
#define META_KEY_MAX (16 + 2 * 1024)

static int sync_metadata(kvspace_t *kv, const char *key, uint8_t ro, uint32_t vid) {
    char meta[META_KEY_MAX];
    if (kvspaceMetaKeyInto(key, meta, sizeof meta) != 0)
        return -1;
    if (ro || vid) {
        uint8_t *value = NULL;
        uint64_t len = 0;
        if (kvspaceMetaEncode(ro, vid, &value, &len) != 0)
            return -1;
        int rc = shm_set_raw(kv, meta, value, (int32_t)len);
        free(value);
        return rc;
    }
    /* ro=0/vid=0 是普通写的常态：没有 meta 就不必进 ART 删除。 */
    if (!art_search(kv, kv->hdr->art_root, (const uint8_t *)meta,
                    (int)strlen(meta)))
        return 0;
    bool removed = false;
    int32_t root = art_del(kv, kv->hdr->art_root,
                           (const uint8_t *)meta, (int)strlen(meta), 0,
                           &removed);
    if (root < 0)
        return -1;                  /* 表满等硬错误：树未变 */
    kv->hdr->art_root = root;
    return 0;
}

int kvspaceShmMetaGetAt(kvspace_t *kv, const char *key, uint8_t *ro,
                        uint32_t *vid) {
    if (!kv || !key || !ro || !vid || kv_sync(kv) != 0)
        return -1;
    *ro = 0;
    *vid = 0;
    char *meta = NULL;
    if (kvspaceMetaKey(key, &meta) != 0)
        return -1;
    art_hdr_t *h = art_search(kv, kv->hdr->art_root,
                              (const uint8_t *)meta, (int)strlen(meta));
    free(meta);
    if (!h)
        return 0;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return 0;
    const uint8_t *data = kv->sbo_data + AV_OFF(hv);
    uint64_t allocated = sbo_allocated_size(kv->sbo_meta, AV_OFF(hv));
    kvspaceXh head;
    if (kvspaceXhDecode(data, allocated, &head) != 0)
        return -1;
    return kvspaceMetaDecode(data, head.total, ro, vid);
}

int kvspaceShmMetaGet(kvspace_t *kv, const char *key, uint8_t *ro,
                      uint32_t *vid) {
    if (!kv || !key || kv_sync(kv) != 0)
        return -1;
    char resolved[1024];
    resolve_path(kv, key, resolved, sizeof resolved);
    return kvspaceShmMetaGetAt(kv, resolved, ro, vid);
}

static int frame_operand_ptr(const char *key, const kvspaceXh *wire) {
    if (wire->kind != (KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG) ||
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
                            const kvspaceXh *wire, uint8_t ro, uint32_t vid) {
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
                !frame_operand_ptr(kbuf, wire)) {
                free(pp);
                free(nn);
                return -1;
            }
        }
        free(pp);
        free(nn);
    }

    /* status 固定档：形状固定（SLACK/headlen=64/[n]char/utf8）、内容 ≤16。
     * 任一不符或现存槽 headlen 不同（旧 store）→ 硬错，不截断、不换 box、
     * 不自动升级。创建时分配下限 80 字节，保证后续合法写入原地完成。 */
    if (is_vthread_status_key(kbuf)) {
        if (wire->content_len > KVSPACE_STATUS_MAX_LEN ||
            wire->kind != KVSPACE_XH_SLACK ||
            wire->headlen != (1u << KVSPACE_XH_POW_SLACK) ||
            !is_status_langtype(wire))
            return -1;
        if (local && AV_HAS(lhv)) {
            uint8_t *old_raw = kv->sbo_data + AV_OFF(lhv);
            uint64_t old_sz = sbo_allocated_size(kv->sbo_meta, AV_OFF(lhv));
            kvspaceXh old;
            /* 旧 store 的不合规 status 槽（形状/尺寸/容量任一不符）：
             * 不自动升级、不 realloc——写入硬错，需重建/重置。 */
            if (kvspaceXhDecode(old_raw, old_sz, &old) != 0 ||
                old.headlen != wire->headlen ||
                old.kind != KVSPACE_XH_SLACK ||
                old.headlen != (1u << KVSPACE_XH_POW_SLACK) ||
                !is_status_langtype(&old) ||
                old.content_len > KVSPACE_STATUS_MAX_LEN ||
                old_sz < KVSPACE_STATUS_MIN_CAP)
                return -1;
        }
    }
    if (shm_set_local(kv, kbuf, val, val_len, local) != 0)
        return -1;
    return sync_metadata(kv, kbuf, ro, vid);
}

int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                  int32_t val_len) {
    if (!kv || !key || !val || val_len <= 0)
        return -1;
    kvspaceXh wire;
    if (kvspaceXhDecode(val, (uint64_t)val_len, &wire) != 0 ||
        wire.total != (uint64_t)val_len)
        return -1;
    return shm_set_validated(kv, key, val, val_len, &wire, 0, 0);
}

int kvspaceShmSetValue(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len, uint8_t ro, uint32_t vid) {
    kvspaceXh head;
    if (!kv || !key || !val || val_len <= 0 ||
        kvspaceXhDecode(val, (uint64_t)val_len, &head) != 0 ||
        head.total != (uint64_t)val_len)
        return -2;
    return shm_set_validated(kv, key, val, val_len, &head, ro, vid);
}

int kvspaceShmSetValueByRef(kvspace_t *kv, kvspaceRef_t *ref,
                            const char *key_fallback, const uint8_t *val,
                            int32_t val_len, uint8_t ro, uint32_t vid) {
    kvspaceXh head;
    if (!kv || !ref || !key_fallback || !val || val_len <= 0 ||
        kvspaceXhDecode(val, (uint64_t)val_len, &head) != 0 ||
        head.total != (uint64_t)val_len)
        return -2;
    if (kv_sync(kv) != 0)
        return -1;
    if (strlen(key_fallback) >= 1024 || strstr(key_fallback, "//") ||
        reserved_meta_path(key_fallback))
        return -1;
    int32_t id = ref_leaf(kv, ref, key_fallback);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (h && AV_HAS(art_val_load(h))) {
        if (shm_set_local(kv, key_fallback, val, val_len, h) != 0)
            return -1;
        return sync_metadata(kv, key_fallback, ro, vid);
    }
    /* ref 过期或尚无值：退回完整按 key 写（含 extindex 检查），成功后重解析。 */
    int rc = shm_set_validated(kv, key_fallback, val, val_len, &head, ro, vid);
    if (rc == 0)
        kvspaceShmResolveRef(kv, key_fallback, ref);
    return rc;
}

int kvspaceShmWriteInPlace(kvspace_t *kv, const char *key, int resolve,
                           int32_t body_len, uint8_t **body) {
    if (!kv || !key || !body || body_len < 0 || reserved_meta_path(key))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
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
    if (!h)
        return -1;
    uint64_t hv = art_val_load(h);
    if (!AV_HAS(hv))
        return -1;
    if ((!fetched && read_value(kv, AV_OFF(hv), &raw, &rl) < 0) || rl <= 0)
        return -1; /* None 或读失败 → 强制走 NewPlace */
    kvspaceXh wire;
    if (kvspaceXhDecode(raw, (uint64_t)rl, &wire) == 0) {
        if (wire.content_len != (uint64_t)body_len)
            return -1;
        *body = raw + wire.headlen;
        return 0;
    }
    return -1;
}

int kvspaceShmWriteNewPlace(kvspace_t *kv, const char *key, uint8_t ref,
                            uint8_t storetype, uint8_t ro, uint32_t vid,
                            const char *langtype, int32_t body_len,
                            uint64_t body_cap,
                            uint8_t **body) {
    if (!kv || !key || !langtype || !body || body_len < 0)
        return -1;
    if (body_cap > INT32_MAX - 32)
        return -1;
    *body = NULL;
    if (kv_sync(kv) != 0)
        return -1;
    size_t key_len = strlen(key);
    if (key_len >= 1024 || strstr(key, "//") || reserved_meta_path(key))
        return -1;
    uint8_t kind;
    if (ref == 0 && storetype <= KVSPACE_XH_FIXED_LARGE)
        kind = storetype;
    else if (ref == 1 && storetype == KVSPACE_XH_SLACK)
        kind = KVSPACE_XH_SLACK | KVSPACE_XH_PTR_FLAG;
    else if (ref == 2 && storetype == KVSPACE_XH_EXT)
        kind = KVSPACE_XH_EXT;
    else
        return -1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXhReserve(kind, langtype, (uint64_t)body_len,
                         body_cap, &value, &total) != 0)
        return -1;
    if (total > INT32_MAX) {
        free(value);
        return -1;
    }
    uint32_t headlen = 1u << value[0];
    int rc = sync_metadata(kv, key, ro, vid);
    if (rc == 0)
        rc = shm_set_raw(kv, key, value, (int32_t)total);
    free(value);
    if (rc != 0)
        return -1;
    art_hdr_t *node = art_search(kv, kv->hdr->art_root,
                                 (const uint8_t *)key, (int)key_len);
    if (!node)
        return -1;
    uint64_t nv = art_val_load(node);
    if (!AV_HAS(nv))
        return -1;
    *body = kv->sbo_data + AV_OFF(nv) + headlen;
    return 0;
}

int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                      int32_t *out_count) {
    char **names;
    int32_t count;
    if (kvspaceShmList(kv, prefix, ex, resolve, &names, &count) != 0) {
        *out_count = 0;
        return -1;
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    *out_count = count;
    return 0;
}

int kvspaceShmDel(kvspace_t *kv, const char *key) {
    if (!kv || !key)
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    char kbuf[1024];
    strncpy(kbuf, key, sizeof(kbuf) - 1);
    kbuf[sizeof(kbuf) - 1] = '\0'; /* 删键本身，不穿透 link——显式解引用由 runtime 掌控 */
    if (sync_metadata(kv, kbuf, 0, 0) != 0)
        return -1;
    bool d = false;
    int32_t root = art_del(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                           (int)strlen(kbuf), 0, &d);
    if (root < 0)
        return -1;                  /* 表满等硬错误：树未变 */
    kv->hdr->art_root = root;
    return d ? 0 : -1;
}

int kvspaceShmDeltree(kvspace_t *kv, const char *prefix) {
    if (!kv || !prefix || reserved_meta_path(prefix))
        return -1;
    if (kv_sync(kv) != 0)
        return -1;
    // if prefix itself is a link, only delete the link
    art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)prefix,
                              (int)strlen(prefix));
    uint64_t hv = h ? art_val_load(h) : 0;
    if (h && AV_HAS(hv)) {
        uint8_t *raw;
        int32_t rl;
        read_value(kv, AV_OFF(hv), &raw, &rl);
        kvspaceXh wire;
        int is_ptr = rl > 0 && kvspaceXhDecode(raw, (uint64_t)rl, &wire) == 0 &&
                     (wire.kind & KVSPACE_XH_PTR_FLAG) != 0;
        if (is_ptr) {
            return kvspaceShmDel(kv, prefix);
        }
    }
    char *e = edir(prefix); // ensure trailing / for listing children
    char **ns;
    int32_t nc;
    kvspaceShmList(kv, e, false, 1, &ns, &nc);
    for (int i = 0; i < nc; i++) {
        char *c = pjoin(e, ns[i]);
        kvspaceShmDeltree(kv, c);
        free(c);
    }
    for (int i = 0; i < nc; i++)
        free(ns[i]);
    free(ns);

    /* 成员目录 marker（prefix·，U+00B7）：成员 key =
       prefix·<name>，直接拼接后递归删除。 slash 版 e=prefix/ 覆盖不到 ·
       成员（json 的 stringkeymap 落盘形态）。 */
    size_t pl = strlen(prefix);
    char *m = malloc(pl + 3);
    memcpy(m, prefix, pl);
    m[pl] = (char)0xC2;
    m[pl + 1] = (char)0xB7;
    m[pl + 2] = 0;
    char **ms;
    int32_t mc;
    kvspaceShmList(kv, m, false, 1, &ms, &mc);
    for (int i = 0; i < mc; i++) {
        size_t ml = strlen(m), nl = strlen(ms[i]);
        char *c = malloc(ml + nl + 1);
        memcpy(c, m, ml);
        memcpy(c + ml, ms[i], nl + 1);
        kvspaceShmDeltree(kv, c);
        free(c);
    }
    for (int i = 0; i < mc; i++)
        free(ms[i]);
    free(ms);
    kvspaceShmDel(kv, m);
    free(m);

    kvspaceShmDel(kv, prefix);
    if (strcmp(e, prefix) != 0)
        kvspaceShmDel(kv, e);
    free(e);
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
    uint8_t ro = 0;
    uint32_t vid = 0;
    if (kvspaceShmMetaGetAt(kv, src, &ro, &vid) != 0) {
        free(tmp);
        return -1;
    }
    int rc = kvspaceShmSet(kv, dst, tmp, rl);
    free(tmp);
    return rc == 0 ? sync_metadata(kv, dst, ro, vid) : rc;
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
    int r = kvspaceXhNewShort("lib", NULL, 0, &v, &vl);
    if (r == 0)
        r = kvspaceShmSetValue(kv, d, v, (int32_t)vl, 0, 0);
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

int kvspaceShmList(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                   char ***on, int32_t *oc) {
    if (!kv || !prefix || !on || !oc)
        return -1;
    *on = NULL;
    *oc = 0;
    if (reserved_meta_path(prefix))
        return 0;
    if (kv_sync(kv) != 0 || bad_dir_prefix(prefix))
        return -1;
    const char *pfx = prefix;
    char tbuf[1024];
    if (resolve) {
        resolve_path(kv, prefix, tbuf, sizeof tbuf);
        pfx = tbuf;
    }
    if (reserved_meta_path(pfx))
        return 0;
    keyscan_t children = {0};
    char buf[4096];
    if (kv->hdr->art_root >= 0 &&
        art_scan_pfx(kv, pfx, (int)strlen(pfx), buf, (int)sizeof buf, &children) != 0)
        goto fail;

    if (ex) {
        char extpath[1024];
        char *dir = edir(pfx);
        if (!dir)
            goto fail;
        int has_ext = dir_ext_path(kv, dir, extpath, sizeof extpath);
        free(dir);
        if (has_ext) {
            if (art_scan_pfx(kv, extpath, (int)strlen(extpath), buf,
                             (int)sizeof buf, &children) != 0)
                goto fail;
        }
    }
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

int kvspaceShmExtindex(kvspace_t *kv, const char *p, const char *ep) {
    if (!kv || !p || !ep)
        return -1;
    int32_t source_len = 0;
    uint8_t *source = kvspaceShmGet(kv, ep, 0, &source_len);
    kvspaceXh source_head;
    if (!source || source_len <= 0 ||
        kvspaceXhDecode(source, (uint64_t)source_len, &source_head) != 0)
        return -1;
    char *type = strndup((const char *)source_head.langtype, source_head.langtype_len);
    if (!type)
        return -1;
    uint8_t *v = NULL;
    uint64_t vl = 0;
    int r = kvspaceXhNewExt(type, ep, strlen(ep), &v, &vl);
    if (r == 0)
        r = kvspaceShmSetValue(kv, p, v, (int32_t)vl, 0, 0);
    free(type);
    free(v);
    return r;
}
int kvspaceShmDelextindex(kvspace_t *kv, const char *p) {
    return kvspaceShmDel(kv, p);
}

/* ============ Watch/Notify ============ */
static int wslot(const char *k) {
    uint32_t h = 5381;
    for (const char *p = k; *p; p++)
        h = ((h << 5) + h) + (uint8_t)*p;
    return (int)(h % WATCH_TABLE_SZ);
}
int kvspaceShmNotify(kvspace_t *kv, const char *k, const uint8_t *v,
                     int32_t vl) {
    if (!kv || !k)
        return -1;
    int s = wslot(k);
    pthread_mutex_lock(&kv->wlock);
    watch_t *w = &kv->watches[s];
    if (w->key[0] && strcmp(w->key, k) == 0) {
        pthread_mutex_lock(&w->mtx);
        free(w->val);
        w->val = v ? memcpy(malloc(vl), v, vl) : NULL;
        w->val_len = vl;
        w->ready = true;
        pthread_cond_signal(&w->cond);
        pthread_mutex_unlock(&w->mtx);
    }
    pthread_mutex_unlock(&kv->wlock);
    return 0;
}
uint8_t *kvspaceShmWatch(kvspace_t *kv, const char *k, int32_t to,
                         int32_t *ol) {
    if (!kv || !k || !ol)
        return NULL;
    *ol = 0;
    int s = wslot(k);
    pthread_mutex_lock(&kv->wlock);
    watch_t *w = &kv->watches[s];
    strncpy(w->key, k, 255);
    w->key[255] = '\0';
    w->ready = false;
    free(w->val);
    w->val = NULL;
    pthread_mutex_unlock(&kv->wlock);
    pthread_mutex_lock(&w->mtx);
    if (!w->ready) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += to / 1000;
        ts.tv_nsec += (to % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&w->cond, &w->mtx, &ts);
    }
    uint8_t *r = NULL;
    if (w->ready && w->val) {
        r = w->val;
        *ol = w->val_len;
        w->val = NULL;
    }
    w->ready = false;
    pthread_mutex_unlock(&w->mtx);
    return r;
}
