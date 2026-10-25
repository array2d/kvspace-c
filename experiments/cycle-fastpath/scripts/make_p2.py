#!/usr/bin/env python3
"""make_p2.py — 生成 P2（版本化 view token）实现树（基于 v0.3.16）。

产物：/tmp/cycle-fastpath/p2/src/kvspace-c（可直接 build/install 给 bench 用）
改动点：
  1. kvspace_hdr_t: gens_size / gen_epoch / format_version；magic → v3
  2. `<path>.gen` 旁路 gen 表（8B/节点：struct_gen + value_gen）+ 随 slab 增长
  3. gen 递增点：art_balloc / art_add / art_resize / 值写 / 值删
  4. shm token API：ViewResolve / ViewResolveFrom / ViewCheck / ViewRefresh + short key 修复
  5. tombstone 删除（延迟释放，供 Del ≤50 验证）
"""
import shutil
import sys
from pathlib import Path

SRC = Path("/tmp/cycle-audit16/src/kvspace-c")
OUT = Path("/tmp/cycle-fastpath/p2/src/kvspace-c")


def sub(s, old, new, n=1, where=""):
    if s.count(old) != n:
        sys.exit(f"anchor mismatch ({s.count(old)} != {n}) {where}: {old[:90]!r}")
    return s.replace(old, new)


def main():
    if not (SRC / "src" / "kvspace.c").exists():
        sys.exit(f"missing {SRC}")
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(SRC, OUT, symlinks=True)
    p = OUT / "src" / "kvspace.c"
    s = p.read_text()

    # ── 1. hdr 字段 + magic v3 ──
    s = sub(s, '#define KVS_MAGIC "kvspace-c.v2"', '#define KVS_MAGIC "kvspace-c.v3"')
    s = sub(s, """    char magic[12];
    uint64_t art_slab_size, sbo_head_size, sbo_data_size;
    int32_t art_root;""", """    char magic[12];
    uint64_t art_slab_size, sbo_head_size, sbo_data_size;
    int32_t art_root;
    /* v3: 旁路 gen 表（节点 id 索引，8B/节点）。 */
    uint64_t gens_size;
    uint64_t gen_epoch;      /* 节点分配 epoch（原子递增） */
    uint32_t format_version; /* 1 = v3 with gens */
    uint32_t recovery_epoch; /* 恢复屏障代（死 pin 作废） */""")

    # ── 2. gen 区域 + 辅助 ──
    s = sub(s, """struct kvspace {""", """#define P2_TOMB_MAX 256

struct kvspace {""")
    s = sub(s, """    shm_region_t r_art, r_head, r_data;
    kvspace_hdr_t *hdr;""", """    shm_region_t r_art, r_head, r_data, r_gen;
    kvspace_hdr_t *hdr;
    uint64_t store_id;   /* 进程内句柄身份（P2 token 用） */
    uint64_t *tomb_ring;          /* P2: 有界 ring（256 × 8B） */
    uint32_t tomb_head, tomb_tail;
    uint64_t tomb_backpressure;
    pthread_t tomb_thread;
    pthread_mutex_t tomb_mu;
    pthread_cond_t tomb_cv;
    int tomb_stop;""")

    helpers = r'''
/* ── P2: 旁路 gen 表（node id → {struct_gen, value_gen}）──────────── */
static void kv_sbo_free(kvspace_t *kv, uint64_t off);
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id);
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b);
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl);
static int32_t art_walk(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                        int d, int32_t *par, int *pard);
static int32_t art_follow(kvspace_t *kv, int32_t id);
static int32_t ref_leaf(kvspace_t *kv, kvspaceRef_t *ref, const char *key);

/* 每节点 16B：packed64 + value_gen。
 * packed = { gen:32 | flags:2 | dead:1 | pending:1 | ref:28 } */
typedef struct {
    uint64_t packed;
    uint32_t value_gen;
    uint32_t _pad;
} kv_gens_t;

#define P2_REF_MASK 0xFFFFFFFull
#define P2_PEND     (1ull << 28)
#define P2_DEAD     (1ull << 29)
#define P2_FLAGS    (3ull << 30)
#define PK_GEN(w)   ((uint32_t)((w) >> 32))
#define PK_REF(w)   ((uint32_t)((w) & P2_REF_MASK))
#define PK_MK(gen, ref, extra) ((((uint64_t)(gen)) << 32) | ((uint64_t)(ref) & P2_REF_MASK) | (extra))

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
    uint64_t n = gen_cap(kv);
    if ((uint64_t)id >= n)
        return NULL;
    return (kv_gens_t *)kv->r_gen.base + id;
}

static void gen_bump_struct(kvspace_t *kv, int32_t id) {
    kv_gens_t *g = gens_at(kv, id);
    if (!g)
        return;
    for (;;) {
        uint64_t w = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);
        uint64_t nw = PK_MK(PK_GEN(w) + 1, PK_REF(w), w & (P2_PEND | P2_DEAD | P2_FLAGS));
        if (__atomic_compare_exchange_n(&g->packed, &w, nw, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
    }
}

static void gen_bump_value(kvspace_t *kv, int32_t id) {
    kv_gens_t *g = gens_at(kv, id);
    if (g)
        __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);
}

static int64_t gen_alloc_epoch(kvspace_t *kv) {
    return (int64_t)__atomic_add_fetch(&kv->hdr->gen_epoch, 1ull, __ATOMIC_ACQ_REL);
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
            if (d == klen)
                return nid;
        }
        nid = art_child(kv, h, (uint8_t)key[d]);
        d++;
    }
    return -1;
}

/* pin：校验 gen + 拒绝 DEAD / (PENDING && ref==0)，与 ref++ 同一 CAS。 */
static int p2_pin(kvspace_t *kv, int32_t id, uint32_t gen) {
    kv_gens_t *g = gens_at(kv, id);
    if (!g)
        return -1;
    for (;;) {
        uint64_t w = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);
        if (PK_GEN(w) != gen || (w & P2_DEAD) ||
            ((w & P2_PEND) && PK_REF(w) == 0))
            return 1;
        uint64_t nw = w + 1;
        if (PK_REF(nw) == 0)
            return 1;
        if (__atomic_compare_exchange_n(&g->packed, &w, nw, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 0;
    }
}

/* unpin：ref--；PENDING 下最后一个 unpin 负责 free（DEAD 的 0→1 是唯一赢家）。 */
static void p2_unpin(kvspace_t *kv, int32_t id, uint64_t off) {
    kv_gens_t *g = gens_at(kv, id);
    if (!g)
        return;
    for (;;) {
        uint64_t w = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);
        if (PK_REF(w) == 0)
            return;
        uint64_t nw = w - 1;
        if (!__atomic_compare_exchange_n(&g->packed, &w, nw, false,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            continue;
        if ((nw & P2_PEND) && PK_REF(nw) == 0 && !(nw & P2_DEAD)) {
            uint64_t dw = nw;
            if (__atomic_compare_exchange_n(&g->packed, &dw, dw | P2_DEAD, false,
                                            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                if (off)
                    kv_sbo_free(kv, off);
            }
        }
        return;
    }
}

/* 表随 ART slab 增长（容量按当前 block 配置重算）。 */
#define GEN_RESERVE (64ULL << 20)   /* 64MiB VA 预留：8M 节点 */
static int gens_grow(kvspace_t *kv) {
    uint64_t want = gen_cap(kv) * sizeof(kv_gens_t);
    if (want <= kv->r_gen.mapped)
        return 0;
    if (want > GEN_RESERVE)
        return -1;
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

'''
    s = sub(s, "static int kv_sync(kvspace_t *kv) {", helpers + "static int kv_sync(kvspace_t *kv) {")

    # open：建/挂 .gen 文件
    s = sub(s, """    char head_path[PATH_MAX], data_path[PATH_MAX];""",
            """    char head_path[PATH_MAX], data_path[PATH_MAX], gen_path[PATH_MAX];""")
    s = sub(s, """        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path)
        return NULL;""", """        snprintf(data_path, sizeof data_path, "%s.sbo.data", path) >=
            (int)sizeof data_path ||
        snprintf(gen_path, sizeof gen_path, "%s.gen", path) >=
            (int)sizeof gen_path)
        return NULL;""")
    s = sub(s, """    kv->r_art.fd = kv->r_head.fd = kv->r_data.fd = -1;""",
            """    kv->r_art.fd = kv->r_head.fd = kv->r_data.fd = kv->r_gen.fd = -1;""")
    s = sub(s, """        (kv->r_data.fd = open(data_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_data, created, data_size, DATA_RESERVE) != 0)
        goto fail;""", """        (kv->r_data.fd = open(data_path, fl, 0644)) < 0 ||
        region_attach(&kv->r_data, created, data_size, DATA_RESERVE) != 0 ||
        (kv->r_gen.fd = open(gen_path, fl, 0644)) < 0)
        goto fail;""")
    # 老 store（v2 magic）已被 magic 检查拒绝；新建时 gen 表随 slab 初始化
    s = sub(s, """    if (created) {
        memset(kv->hdr, 0, sizeof(*kv->hdr));""", """    kv->store_id = ((uint64_t)getpid() << 32) ^ (uint64_t)(uintptr_t)kv;

    if (created) {
        memset(kv->hdr, 0, sizeof(*kv->hdr));
        kv->hdr->format_version = 1;""")

    # close：解映射
    s = sub(s, """static void region_close(shm_region_t *r) {""",
            """static void region_close(shm_region_t *r); /* fwd */

static void region_close(shm_region_t *r) {""")

    # ── 3. gen 递增点 ──
    s = sub(s, """static int32_t art_balloc(kvspace_t *kv) {
    int64_t id = blocks_alloc(kv->art_meta, kv->art_data);
    if (id >= 0)
        return (int32_t)id;
    if (art_slab_grow(kv) != 0)
        return -1;
    return (int32_t)blocks_alloc(kv->art_meta, kv->art_data);
}""", """static int32_t art_balloc(kvspace_t *kv) {
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
            __atomic_store_n(&g->packed,
                             PK_MK((uint32_t)gen_alloc_epoch(kv), 0, 0),
                             __ATOMIC_RELEASE);
            __atomic_store_n(&g->value_gen, 0u, __ATOMIC_RELEASE);
        }
    }
    return (int32_t)id;
}""")

    # art_add：父节点结构变化
    s = sub(s, """/* ---- add child ---- */
static int art_add(kvspace_t *kv, void *n, uint8_t b, int32_t cid) {""",
            """/* ---- add child ---- */
static int art_add(kvspace_t *kv, void *n, uint8_t b, int32_t cid) {
    gen_bump_struct(kv, art_id_of(kv, n)); /* P2: 子集变化 */""")

    # art_resize：新节点继承 +1
    s = sub(s, """    oh->type = ART_MOVED;
    oh->has_value = 0;
    oh->box_offset = (uint64_t)(uint32_t)nid;
    return nid;""", """    {   /* P2: 搬迁 —— 新节点 struct_gen = 旧 +1；旧节点也动一次 */
        int32_t oid = art_id_of(kv, oh);
        (void)gens_at;
        gen_bump_struct(kv, nid);   /* 新节点 gen 先抬一代 */
        gen_bump_struct(kv, oid);   /* 旧节点也失效 */
    }
    oh->type = ART_MOVED;
    oh->has_value = 0;
    oh->box_offset = (uint64_t)(uint32_t)nid;
    return nid;""")

    # 值写（shm_set_local 两分支）
    s = sub(s, """        uint64_t cap = sbo_allocated_size(kv->sbo_meta, old->box_offset);
        if ((uint64_t)val_len <= cap) {
            memcpy(kv->sbo_data + old->box_offset, val, (size_t)val_len);
            return 0;
        }
        kv_sbo_free(kv, old->box_offset);""", """        uint64_t cap = sbo_allocated_size(kv->sbo_meta, old->box_offset);
        if ((uint64_t)val_len <= cap) {
            /* P2: 同尺寸就地写 —— 只 bump value_gen，指针不变 */
            memcpy(kv->sbo_data + old->box_offset, val, (size_t)val_len);
            gen_bump_value(kv, art_id_of(kv, old));
            return 0;
        }
        kv_sbo_free(kv, old->box_offset);""")

    # 值删：art_del 叶子
    s = sub(s, """        h->has_value = 0;
        kv_sbo_free(kv, h->box_offset);
        h->box_offset = 0;
        *del = true;""", """        h->has_value = 0;
        gen_bump_value(kv, art_id_of(kv, h)); /* P2: 值删 */
        gen_bump_struct(kv, art_id_of(kv, h)); /* P2: 槽消失，view 失效 */
        kv_sbo_free_deferred(kv, h->box_offset);
        h->box_offset = 0;
        *del = true;""")

    # ── 4. 延迟释放 + token API ──
    extra = r'''
/* ── P2: tombstone 延迟释放（粗粒度 epoch：队满或显式 flush 时统一 free）── */
static void kv_sbo_free_deferred(kvspace_t *kv, uint64_t off);
static void p2_flush_tomb(kvspace_t *kv);


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
    int32_t id = art_follow(kv, (int32_t)ref->block_id);
    art_hdr_t *h = id >= 0 ? art_hdr(kv, id) : NULL;
    if (!h || !h->has_value)
        return -1;
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, h->box_offset);
    if (cap < 1)
        return -1;
    uint8_t *raw = kv->sbo_data + h->box_offset;
    kvspaceXh w;
    if (kvspaceXhHeadView(raw, cap, &w) != 0 || w.total > cap ||
        w.kind == KVSPACE_XH_EXT || (w.kind & KVSPACE_XH_PTR_FLAG))
        return -1;
    kv_gens_t *g = gens_at(kv, id);
    if (!g)
        return -1;
    uint64_t pw = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);
    uint32_t gen = PK_GEN(pw);
    int pr = p2_pin(kv, id, gen);
    if (pr != 0)
        return pr == 1 ? 1 : -1;      /* 1 = STALE */
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
    if (tok->store_id != kv->store_id)
        return 1;
    if (tok->_pad != __atomic_load_n(&kv->hdr->recovery_epoch, __ATOMIC_ACQUIRE))
        return 1;
    kv_gens_t *g = gens_at(kv, (int32_t)tok->node_id);
    if (!g)
        return 1;
    uint64_t pw = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);
    if (PK_GEN(pw) != tok->struct_gen || (pw & P2_DEAD))
        return 1;
    return 0;
}

/* 释放 token 的 pin（runtime 在帧销毁 / STALE / evict 时调用）。 */
int kvspaceShmViewRelease(kvspace_t *kv, kvspaceViewToken_t *tok) {
    if (!kv || !tok)
        return -1;
    if (tok->store_id != kv->store_id)
        return 1;
    uint64_t off = 0;
    if (tok->head && kv->sbo_data && tok->head >= kv->sbo_data)
        off = (uint64_t)(tok->head - kv->sbo_data);
    p2_unpin(kv, (int32_t)tok->node_id, off);
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
    if (!g || !h || !h->has_value)
        return 1;
    /* 叶子级逻辑墓碑：键立刻不存在；不收 slot、不 bump struct_gen、不入 ring。
     * 读者靠 value_gen 检测删除；回收交给后台 sweep。 */
    h->has_value = 0;
    g->value_gen++;                 /* 单写者：release fence 见下 */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    (void)key;
    return 0;
}

/* 后台 sweep：DFS 扫描墓碑叶（has_value==0 且 box 未回收），释放 box 并 bump struct_gen。
 * 调用方保证不与并发写同 key；返回释放数。 */
static int p2_sweep_node(kvspace_t *kv, int32_t nid) {
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h || h->type == ART_MOVED)
        return 0;
    int freed = 0;
    if (!h->has_value && h->box_offset) {
        uint64_t off = h->box_offset;
        gen_bump_struct(kv, nid);          /* 先 release 抬 gen：旧 token 全部 STALE */
        kv_gens_t *g = gens_at(kv, nid);
        int here = 0;
        if (g) {
            for (;;) {
                uint64_t w = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);
                if (w & P2_DEAD)
                    break;
                if (PK_REF(w) > 0) {
                    if (__atomic_compare_exchange_n(&g->packed, &w, w | P2_PEND,
                                                    false, __ATOMIC_ACQ_REL,
                                                    __ATOMIC_ACQUIRE))
                        break;
                } else {
                    if (__atomic_compare_exchange_n(&g->packed, &w, w | P2_DEAD,
                                                    false, __ATOMIC_ACQ_REL,
                                                    __ATOMIC_ACQUIRE)) {
                        kv_sbo_free(kv, off);
                        here = 1;
                    }
                    break;
                }
            }
        } else {
            kv_sbo_free(kv, off);
            here = 1;
        }
        h->box_offset = 0;
        freed += here;
    }
    switch (h->type) {
    case ART_N4: {
        art_n4_t *x = (art_n4_t *)h;
        for (int i = 0; i < (int)h->count; i++)
            freed += p2_sweep_node(kv, x->children[i]);
        break;
    }
    case ART_N16: {
        art_n16_t *x = (art_n16_t *)h;
        for (int i = 0; i < (int)h->count; i++)
            freed += p2_sweep_node(kv, x->children[i]);
        break;
    }
    case ART_N48: {
        art_n48_t *x = (art_n48_t *)h;
        for (int i = 0; i < 256; i++)
            if (x->index[i] != 255)
                freed += p2_sweep_node(kv, x->children[x->index[i]]);
        break;
    }
    case ART_N256: {
        art_n256_t *x = (art_n256_t *)h;
        for (int i = 0; i < 256; i++)
            freed += p2_sweep_node(kv, x->children[i]);
        break;
    }
    }
    return freed;
}

int kvspaceShmViewSweep(kvspace_t *kv) {
    if (!kv || kv->hdr->art_root < 0)
        return 0;
    return p2_sweep_node(kv, kv->hdr->art_root);
}

int kvspaceShmViewFlushTomb(kvspace_t *kv) {
    if (!kv)
        return -1;
    p2_flush_tomb(kv);
    return 0;
}

int kvspaceShmViewRefresh(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key) {
    kvspaceViewToken_t t;
    if (kvspaceShmViewResolve(kv, key, &t) != 0)
        return -1;
    *tok = t;
    return 0;
}
'''
    s = sub(s, "static int kv_sync(kvspace_t *kv) {", extra + "\nstatic int kv_sync(kvspace_t *kv) {")

    # tombstone 实现（放在 kv_sbo_free 之后）
    s = sub(s, """    sbo_free(kv->sbo_meta, off);
}""", """    sbo_free(kv->sbo_meta, off);
}

/* P2: tombstone —— 有界 ring（L1 内）+ 后台 flush 线程。
 * 前台只做：ring[tail & MASK] = off; tail++（release）; 唤醒后台。
 * 队列满时前台等待（backpressure），绝不在调用线程里 sbo_free。 */
#define P2_RING 256
static void *p2_flusher(void *arg);

static void kv_sbo_free_deferred(kvspace_t *kv, uint64_t off) {
    if (off == 0 || (int64_t)off < 0)
        return;
    for (;;) {
        uint32_t t = __atomic_load_n(&kv->tomb_tail, __ATOMIC_RELAXED);
        uint32_t h = __atomic_load_n(&kv->tomb_head, __ATOMIC_ACQUIRE);
        if (t - h < P2_RING) {
            kv->tomb_ring[t & (P2_RING - 1)] = off;
            __atomic_store_n(&kv->tomb_tail, t + 1, __ATOMIC_RELEASE);
            break;
        }
        /* 满：唤醒后台并等待空位（backpressure） */
        __atomic_add_fetch(&kv->tomb_backpressure, 1ull, __ATOMIC_RELAXED);
        pthread_mutex_lock(&kv->tomb_mu);
        pthread_cond_signal(&kv->tomb_cv);
        pthread_mutex_unlock(&kv->tomb_mu);
        sched_yield();
    }
    pthread_mutex_lock(&kv->tomb_mu);
    pthread_cond_signal(&kv->tomb_cv);
    pthread_mutex_unlock(&kv->tomb_mu);
}

static void p2_flush_tomb(kvspace_t *kv) {
    for (;;) {
        uint32_t h = __atomic_load_n(&kv->tomb_head, __ATOMIC_ACQUIRE);
        uint32_t t = __atomic_load_n(&kv->tomb_tail, __ATOMIC_ACQUIRE);
        if (h == t)
            break;
        uint64_t off = kv->tomb_ring[h & (P2_RING - 1)];
        __atomic_store_n(&kv->tomb_head, h + 1, __ATOMIC_RELEASE);
        kv_sbo_free(kv, off);
    }
}

static void *p2_flusher(void *arg) {
    kvspace_t *kv = arg;
    for (;;) {
        pthread_mutex_lock(&kv->tomb_mu);
        while (!kv->tomb_stop &&
               __atomic_load_n(&kv->tomb_head, __ATOMIC_ACQUIRE) ==
                   __atomic_load_n(&kv->tomb_tail, __ATOMIC_ACQUIRE))
            pthread_cond_wait(&kv->tomb_cv, &kv->tomb_mu);
        int stop = kv->tomb_stop;
        pthread_mutex_unlock(&kv->tomb_mu);
        p2_flush_tomb(kv);
        if (stop)
            break;
    }
    return NULL;
}""")

    # open：gen 表在 blocks_init 之后映射
    s = sub(s, """    /* growth code assumes the single-root pool layout (SBO_HEAD_FIXED) */""",
            """    if (gens_grow(kv) != 0)   /* P2: gen 表在 blocks_init 之后 */
        goto fail;

    /* growth code assumes the single-root pool layout (SBO_HEAD_FIXED) */""")

    # gen 文件 close
    s = sub(s, """    region_close(&kv->r_data);""", """    if (kv->tomb_ring) {
        pthread_mutex_lock(&kv->tomb_mu);
        kv->tomb_stop = 1;
        pthread_cond_signal(&kv->tomb_cv);
        pthread_mutex_unlock(&kv->tomb_mu);
        pthread_join(kv->tomb_thread, NULL);
        p2_flush_tomb(kv);
        free(kv->tomb_ring);
    }
    region_close(&kv->r_data);
    region_close(&kv->r_gen);""", n=2)

    # short key 修复：ref_leaf 允许 d == klen
    s = sub(s, """    int d = (int)ref->gen;
    if (d < 0 || d >= klen)
        return -1;""", """    int d = (int)ref->gen;
    if (d < 0 || d > klen)   /* P2: d == klen 表示前缀节点就是全部路径 */
        return -1;
    if (d == klen)
        return art_follow(kv, (int32_t)ref->block_id);""")

    # P2: head-only 解码（写入时仍全量校验）
    hh = OUT / "src" / "xvalue_head.h"
    sh = hh.read_text()
    sh = sh.replace("int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out);",
                    "int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out);\n"
                    "int kvspaceXhHeadView(const uint8_t *data, uint64_t len, kvspaceXh *out);")
    hh.write_text(sh)
    hc = OUT / "src" / "xvalue_head.c"
    sc = hc.read_text()
    sc = sc.replace("int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out) {",
                    "int kvspaceXhHeadView(const uint8_t *data, uint64_t len, kvspaceXh *out) {\n"
                    "    return xh_header(data, len, out);\n"
                    "}\n\n"
                    "int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out) {")
    hc.write_text(sc)

    # P2: 内部头补 token 类型与 shm 原型；durable_abi 补 ABI wrappers
    sh = OUT / "src" / "kvspace_shm.h"
    hs = sh.read_text()
    hs = hs.replace("#endif", """#ifndef KVSPACE_VIEW_TOKEN_DEFINED
#define KVSPACE_VIEW_TOKEN_DEFINED
typedef struct {
    uint8_t *head; uint8_t *body; uint32_t headlen; uint32_t value_len;
    uint64_t cap; uint64_t body_cap;
    uint32_t struct_gen; uint32_t value_gen;
    uint64_t store_id; uint32_t node_id; uint32_t _pad;
} kvspaceViewToken_t;
#endif
int kvspaceShmResolvePrefix(kvspace_t *kv, const char *path, kvspaceRef_t *ref);
int kvspaceShmViewResolve(kvspace_t *kv, const char *key, kvspaceViewToken_t *tok);
int kvspaceShmViewFromRef(kvspace_t *kv, const kvspaceRef_t *ref,
                          const char *key_fallback, kvspaceViewToken_t *tok);
int kvspaceShmViewResolveFrom(kvspace_t *kv, const kvspaceRef_t *parent,
                              const char *rel, kvspaceViewToken_t *tok);
int kvspaceShmViewCheck(kvspace_t *kv, const kvspaceViewToken_t *tok);
int kvspaceShmViewWrite(kvspace_t *kv, kvspaceViewToken_t *tok,
                        const uint8_t *data, uint32_t len);
int kvspaceShmViewDel(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key);
int kvspaceShmViewRefresh(kvspace_t *kv, kvspaceViewToken_t *tok, const char *key);
int kvspaceShmViewRelease(kvspace_t *kv, kvspaceViewToken_t *tok);
int kvspaceShmViewSweep(kvspace_t *kv);
int kvspaceShmViewFlushTomb(kvspace_t *kv);

#endif""")
    sh.write_text(hs)

    da = OUT / "src" / "durable_abi.c"
    s2 = da.read_text()
    s2 += """

/* ── P2 token ABI wrappers（durable 后端后续给 ENOTSUP）──────────── */
int kvspaceViewResolvePrefix(void *h, const char *path, kvspaceRef_t *ref) {
    return kvspaceShmResolvePrefix((kvspace_t *)h, path, ref) == 0 ? 0 : 1;
}

int kvspaceViewRelease(void *h, kvspaceViewToken_t *tok) {
    return kvspaceShmViewRelease((kvspace_t *)h, tok);
}

int kvspaceViewSweep(void *h) {
    return kvspaceShmViewSweep((kvspace_t *)h);
}

int kvspaceViewResolve(void *h, const char *key, kvspaceViewToken_t *tok) {
    return kvspaceShmViewResolve((kvspace_t *)h, key, tok) == 0 ? 0 : 1;
}
int kvspaceViewFromRef(void *h, const kvspaceRef_t *ref, const char *k,
                       kvspaceViewToken_t *tok) {
    return kvspaceShmViewFromRef((kvspace_t *)h, ref, k, tok) == 0 ? 0 : 1;
}
int kvspaceViewResolveFrom(void *h, const kvspaceRef_t *parent, const char *rel,
                           kvspaceViewToken_t *tok) {
    return kvspaceShmViewResolveFrom((kvspace_t *)h, parent, rel, tok) == 0 ? 0 : 1;
}
int kvspaceViewCheck(void *h, const kvspaceViewToken_t *tok) {
    return kvspaceShmViewCheck((kvspace_t *)h, tok);
}
int kvspaceViewWrite(void *h, kvspaceViewToken_t *tok, const uint8_t *d, uint32_t n) {
    return kvspaceShmViewWrite((kvspace_t *)h, tok, d, n);
}
int kvspaceViewDel(void *h, kvspaceViewToken_t *tok, const char *key) {
    return kvspaceShmViewDel((kvspace_t *)h, tok, key);
}
int kvspaceViewRefresh(void *h, kvspaceViewToken_t *tok, const char *key) {
    return kvspaceShmViewRefresh((kvspace_t *)h, tok, key);
}
"""
    da.write_text(s2)

    # open：tomb ring + 后台 flusher（必须在 return kv 之前）
    s = sub(s, """    pthread_mutex_init(&kv->wlock, NULL);""",
            """    kv->tomb_ring = calloc(P2_RING, sizeof(uint64_t));
    if (!kv->tomb_ring)
        goto fail;
    pthread_mutex_init(&kv->tomb_mu, NULL);
    pthread_cond_init(&kv->tomb_cv, NULL);
    if (pthread_create(&kv->tomb_thread, NULL, p2_flusher, kv) != 0)
        goto fail;

    pthread_mutex_init(&kv->wlock, NULL);""")

    # P2: 行级插入复活重置（对格式鲁棒）
    _ls = s.split("\n")
    _out = []
    _done = False
    for _i, _l in enumerate(_ls):
        if (not _done and "kv->hdr->art_root = art_ins(kv, kv->hdr->art_root" in _l):
            # 找到紧随其后的 "    return 0;"
            _k = _i
            while _k < len(_ls) and _ls[_k].strip() != "return 0;":
                _k += 1
            if _k < len(_ls):
                _out.extend(_ls[_i:_k])
                _out.extend([
                    "    /* P2: 换 box / 墓碑复活——清 pending/dead 并抬 gen（旧 token 失效）。 */",
                    "    {",
                    "        int32_t nid = p2_node_at(kv, key);",
                    "        kv_gens_t *g = gens_at(kv, nid);",
                    "        if (g) {",
                    "            uint64_t w = __atomic_load_n(&g->packed, __ATOMIC_ACQUIRE);",
                    "            __atomic_store_n(&g->packed, PK_MK(PK_GEN(w) + 1, 0, 0), __ATOMIC_RELEASE);",
                    "            __atomic_add_fetch(&g->value_gen, 1u, __ATOMIC_ACQ_REL);",
                    "        }",
                    "    }",
                ])
                _ls = _ls[_k:]
                _done = True
                _out.extend(_ls)
                break
        _out.append(_l)
    s = "\n".join(_out)
    assert "墓碑复活" in s, "reset insertion failed"

    p.write_text(s)
    print(f"patched {p}")
    print("NOTE: kvspaceViewToken_t / 公共符号声明需在 xvalue 头或公共 ABI 头补；见 patches/。")


if __name__ == "__main__":
    main()
