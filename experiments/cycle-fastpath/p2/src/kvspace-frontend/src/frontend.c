/* frontend.c — KVSpace dispatch 前端。
 *
 * 导出 kvspace* C ABI（与两个后端同名同签名）。运行期按 DSN scheme 用 dlopen
 * （RTLD_NOW | RTLD_LOCAL）装载后端，把 handle 包一层 {vtable, dl, backend}。
 * codec（XvalueEncode、DecodeHead、New 等）与 kvspaceConst 无 handle，由前端静态实现，byte-identical。
 *
 * 后端装载名（后缀随平台）：
 *   shm://...           → libkvspace-c.so.1       / macOS: libkvspace-c.dylib
 *   其余（redis/fs/s3） → libkvspace_durable.so.1 / macOS: libkvspace_durable.dylib
 * 目录由 KVSPACE_BACKEND_PATH 覆盖，默认 Linux /usr/lib/kvspace、macOS /usr/local/lib/kvspace。
 */

#include "kvspace/kvspace.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── 后端 vtable（仅 handle 相关符号；codec 由前端静态实现） ─────────── */

typedef struct {
    void (*free)(void *h);
    int  (*get)(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len);
    int  (*setvalue)(void *h, const char *key, const uint8_t *value, uint32_t value_len,
                     uint8_t ro, uint32_t vid, char *err, uint32_t err_cap);
    int  (*getpart)(void *h, const char *key, uint32_t offset, uint32_t len,
                     uint8_t **out, uint32_t *out_len);
    int  (*setpart)(void *h, const char *key, uint32_t offset, const uint8_t *buf,
                     uint32_t buf_len, char *err, uint32_t err_cap);
    int  (*gethead)(void *h, const char *key, kvspaceHead_t *out);
    int  (*writeinplace)(void *h, const char *key, int resolve, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap);
    int  (*writenewplace)(void *h, const char *key, uint8_t ref, uint8_t storetype,
                          uint8_t ro, uint32_t vid, const char *langtype, uint32_t body_len,
                          uint64_t body_cap,
                          uint8_t **body, char *err, uint32_t err_cap);
    int  (*listlen)(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count);
    int  (*listat)(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                   uint8_t *buf, uint32_t buf_cap, uint32_t *out_len);
    int  (*del)(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
    int  (*deltree)(void *h, const char *prefix, char *err, uint32_t err_cap);
    int  (*cp)(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
    int  (*cptree)(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
    int  (*cplist)(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
    int  (*mkindex)(void *h, const char *path, uint32_t capacity, char *err, uint32_t err_cap);
    int  (*mkindexext)(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap);
    int  (*rmindexext)(void *h, const char *path, char *err, uint32_t err_cap);
    int  (*clear)(void *h, char *err, uint32_t err_cap);
    int  (*watch)(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                  uint64_t tick_ns, uint8_t **out, uint32_t *out_len);
    int  (*resolveref)(void *h, const char *key, kvspaceRef_t *ref);
    int  (*getbyref)(void *h, kvspaceRef_t *ref, const char *key_fallback,
                     uint8_t **out, uint32_t *out_len);
    int  (*setpartbyref)(void *h, kvspaceRef_t *ref, const char *key_fallback,
                         uint32_t offset, const uint8_t *buf, uint32_t buf_len,
                         char *err, uint32_t err_cap);
    int  (*setvaluebyref)(void *h, kvspaceRef_t *ref, const char *key_fallback,
                          const uint8_t *value, uint32_t value_len, uint8_t ro,
                          uint32_t vid, char *err, uint32_t err_cap);
    int  (*slotview)(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                     kvspaceSlotView_t *out);
    int  (*viewsweep)(void *h);
    int  (*viewresolveprefix)(void *h, const char *path, kvspaceRef_t *ref);
    int  (*viewresolve)(void *h, const char *key, kvspaceViewToken_t *tok);
    int  (*viewfromref)(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                        kvspaceViewToken_t *tok);
    int  (*viewresolvefrom)(void *h, const kvspaceRef_t *parent, const char *rel,
                            kvspaceViewToken_t *tok);
    int  (*viewcheck)(void *h, const kvspaceViewToken_t *tok);
    int  (*viewwrite)(void *h, kvspaceViewToken_t *tok, const uint8_t *data, uint32_t len);
    int  (*viewdel)(void *h, kvspaceViewToken_t *tok, const char *key);
    int  (*viewrefresh)(void *h, kvspaceViewToken_t *tok, const char *key);
    int  (*viewack)(void *h, uint32_t *epoch);
    int  (*viewquiesce)(void *h, int on);
} kvspace_vt;

typedef struct kvspace_handle {
    kvspace_vt *vt;
    void       *dl;
    void       *backend;
    struct kvspace_handle *next;
} kvspace_handle;

static kvspace_handle *H(void *h) { return (kvspace_handle *)h; }

/* 活跃句柄表：durable 的写侧惰性（body 由调用方在返回后填，落盘延到下一次 op），故漏调
 * Close 就退出会丢最后一笔写。进程正常退出时前端兜底落盘再关闭，兑现 kvspace.h 的退出保证。 */
static kvspace_handle  *live;
static int              live_hooked;
static pthread_mutex_t  live_lock = PTHREAD_MUTEX_INITIALIZER;

static void handle_free(kvspace_handle *x) {
    x->vt->free(x->backend);
    dlclose(x->dl);
    free(x->vt);
    free(x);
}

static void live_unlink(kvspace_handle *x) {
    pthread_mutex_lock(&live_lock);
    for (kvspace_handle **p = &live; *p; p = &(*p)->next)
        if (*p == x) { *p = x->next; break; }
    pthread_mutex_unlock(&live_lock);
}

/* 退出兜底：未 Close 句柄逐个关闭——durable 的 Close 落盘未决写，落盘失败由后端自己写
 * stderr。数据可以丢（后端故障），但绝不静默。 */
static void exit_close(void) {
    for (;;) {
        pthread_mutex_lock(&live_lock);
        kvspace_handle *x = live;
        if (x) live = x->next;
        pthread_mutex_unlock(&live_lock);
        if (!x) return;
        handle_free(x);
    }
}

/* ── 后端选择 ───────────────────────────────────────────────────────── */

/* 后端库名：Linux 为 .so.1，macOS 为 .dylib。 */
static const char *backend_soname(const char *dsn) {
    int is_shm = dsn && strncmp(dsn, "shm://", 6) == 0;
#if defined(__APPLE__)
    return is_shm ? "libkvspace-c.dylib" : "libkvspace_durable.dylib";
#else
    return is_shm ? "libkvspace-c.so.1" : "libkvspace_durable.so.1";
#endif
}

/* 后端目录默认值：Linux /usr/lib/kvspace；macOS /usr/local/lib/kvspace（/usr 受 SIP 保护）。
 * KVSPACE_BACKEND_PATH 可覆盖。 */
static char *backend_path(const char *soname, char *buf, size_t cap) {
    const char *dir = getenv("KVSPACE_BACKEND_PATH");
    if (!dir || !dir[0]) {
#if defined(__APPLE__)
        dir = "/usr/local/lib/kvspace";
#else
        dir = "/usr/lib/kvspace";
#endif
    }
    snprintf(buf, cap, "%s/%s", dir, soname);
    return buf;
}

/* ── 常量查询（const.h 的 X 宏表生成查找表） ───────────────────────── */

#define KVSPACE_ENTRY(name) { #name, name },
static const struct { const char *key; const char *val; } kvspace_consts[] = {
    KVSPACE_KV(KVSPACE_ENTRY)
};
#undef KVSPACE_ENTRY

const char *kvspaceConst(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof kvspace_consts / sizeof kvspace_consts[0]; i++)
        if (strcmp(kvspace_consts[i].key, name) == 0)
            return kvspace_consts[i].val;
    return NULL;
}

/* ── 生命周期 ───────────────────────────────────────────────────────── */

void *kvspaceConnect(const char *dsn) {
    if (!dsn) return NULL;
    char path[4096];
    backend_path(backend_soname(dsn), path, sizeof path);

    void *dl = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!dl) return NULL;

    kvspace_vt *vt = calloc(1, sizeof(*vt));
    if (!vt) { dlclose(dl); return NULL; }

    #define LOAD(field, name) do {                                  \
        *(void **)&(vt)->field = dlsym(dl, name);                   \
        if (!(vt)->field) { free(vt); dlclose(dl); return NULL; }   \
    } while (0)

    LOAD(free, "kvspaceClose");
    LOAD(get, "kvspaceGet");
    LOAD(setvalue, "kvspaceSetValue");
    LOAD(getpart, "kvspaceGetPart");
    LOAD(setpart, "kvspaceSetPart");
    LOAD(gethead, "kvspaceGetHead");
    LOAD(writeinplace, "kvspaceWriteInPlace");
    LOAD(writenewplace, "kvspaceWriteNewPlace");
    LOAD(listlen, "kvspaceListLen");
    LOAD(listat, "kvspaceListAt");
    LOAD(del, "kvspaceDel");
    LOAD(deltree, "kvspaceDelTree");
    LOAD(cp, "kvspaceCp");
    LOAD(cptree, "kvspaceCpTree");
    LOAD(cplist, "kvspaceCpList");
    LOAD(mkindex, "kvspaceMkindex");
    LOAD(mkindexext, "kvspaceMkindexExt");
    LOAD(rmindexext, "kvspaceRmindexExt");
    LOAD(clear, "kvspaceClear");
    LOAD(watch, "kvspaceWatch");
    LOAD(slotview, "kvspaceSlotView");
    #undef LOAD
    *(void **)&vt->resolveref = dlsym(dl, "kvspaceResolveRef");
    *(void **)&vt->getbyref = dlsym(dl, "kvspaceGetByRef");
    *(void **)&vt->setpartbyref = dlsym(dl, "kvspaceSetPartByRef");
    *(void **)&vt->setvaluebyref = dlsym(dl, "kvspaceSetValueByRef");
    /* P2 可选符号：缺失时 trampoline 返回 -2 */
    *(void **)&vt->viewsweep = dlsym(dl, "kvspaceViewSweep");
    *(void **)&vt->viewresolveprefix = dlsym(dl, "kvspaceViewResolvePrefix");
    *(void **)&vt->viewresolve = dlsym(dl, "kvspaceViewResolve");
    *(void **)&vt->viewfromref = dlsym(dl, "kvspaceViewFromRef");
    *(void **)&vt->viewresolvefrom = dlsym(dl, "kvspaceViewResolveFrom");
    *(void **)&vt->viewcheck = dlsym(dl, "kvspaceViewCheck");
    *(void **)&vt->viewwrite = dlsym(dl, "kvspaceViewWrite");
    *(void **)&vt->viewdel = dlsym(dl, "kvspaceViewDel");
    *(void **)&vt->viewrefresh = dlsym(dl, "kvspaceViewRefresh");
    *(void **)&vt->viewack = dlsym(dl, "kvspaceViewAck");
    *(void **)&vt->viewquiesce = dlsym(dl, "kvspaceViewQuiesce");

    void *(*connect)(const char *) = dlsym(dl, "kvspaceConnect");
    if (!connect) { free(vt); dlclose(dl); return NULL; }
    void *backend = connect(dsn);
    if (!backend) { free(vt); dlclose(dl); return NULL; }

    kvspace_handle *h = malloc(sizeof(*h));
    if (!h) { vt->free(backend); free(vt); dlclose(dl); return NULL; }
    h->vt = vt; h->dl = dl; h->backend = backend;

    pthread_mutex_lock(&live_lock);
    h->next = live;
    live = h;
    if (!live_hooked) { live_hooked = 1; atexit(exit_close); }
    pthread_mutex_unlock(&live_lock);
    return h;
}

void kvspaceClose(void *h) {
    if (!h) return;
    kvspace_handle *x = H(h);
    live_unlink(x);
    handle_free(x);
}

/* ── 单点读写 / 目录（trampoline） ─────────────────────────────────── */

int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->get(x->backend, key, resolve, out, out_len);
}

int kvspaceSetValue(void *h, const char *key, const uint8_t *value, uint32_t value_len,
                    uint8_t ro, uint32_t vid, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->setvalue(x->backend, key, value, value_len, ro, vid, err, err_cap);
}

int kvspaceResolveRef(void *h, const char *key, kvspaceRef_t *ref) {
    kvspace_handle *x = H(h);
    if (!x->vt->resolveref) return 1;
    return x->vt->resolveref(x->backend, key, ref);
}

int kvspaceGetByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                    uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    if (x->vt->getbyref)
        return x->vt->getbyref(x->backend, ref, key_fallback, out, out_len);
    if (!key_fallback) { *out = NULL; *out_len = 0; return 0; }
    return x->vt->get(x->backend, key_fallback, 0, out, out_len);
}

int kvspaceSetPartByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                        uint32_t offset, const uint8_t *buf, uint32_t buf_len,
                        char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    if (x->vt->setpartbyref)
        return x->vt->setpartbyref(x->backend, ref, key_fallback, offset, buf,
                                   buf_len, err, err_cap);
    if (err && err_cap)
        snprintf(err, err_cap, "kvspace: set-part-by-ref unsupported");
    return 1;
}

int kvspaceSlotView(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                    kvspaceSlotView_t *out) {
    kvspace_handle *x = H(h);
    return x->vt->slotview(x->backend, ref, key_fallback, out);
}

int kvspaceSetValueByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                         const uint8_t *value, uint32_t value_len, uint8_t ro,
                         uint32_t vid, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    if (x->vt->setvaluebyref)
        return x->vt->setvaluebyref(x->backend, ref, key_fallback, value,
                                    value_len, ro, vid, err, err_cap);
    if (!key_fallback)
        return 1;
    return x->vt->setvalue(x->backend, key_fallback, value, value_len, ro, vid,
                           err, err_cap);
}

int kvspaceGetPart(void *h, const char *key, uint32_t offset, uint32_t len,
                    uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->getpart(x->backend, key, offset, len, out, out_len);
}

int kvspaceSetPart(void *h, const char *key, uint32_t offset, const uint8_t *buf,
                    uint32_t buf_len, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->setpart(x->backend, key, offset, buf, buf_len, err, err_cap);
}

int kvspaceGetHead(void *h, const char *key, kvspaceHead_t *out) {
    kvspace_handle *x = H(h);
    return x->vt->gethead(x->backend, key, out);
}

int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->writeinplace(x->backend, key, resolve, body_len, body, err, err_cap);
}

int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref, uint8_t storetype,
                         uint8_t ro, uint32_t vid, const char *langtype, uint32_t body_len,
                         uint64_t body_cap,
                         uint8_t **body, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->writenewplace(x->backend, key, ref, storetype, ro, vid, langtype,
                                body_len, body_cap, body, err, err_cap);
}

int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count) {
    kvspace_handle *x = H(h);
    return x->vt->listlen(x->backend, prefix, expand_ext, resolve, out_count);
}

int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                  uint8_t *buf, uint32_t buf_cap, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->listat(x->backend, prefix, expand_ext, resolve, idx, buf, buf_cap, out_len);
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->del(x->backend, keys, nkeys, err, err_cap);
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->deltree(x->backend, prefix, err, err_cap);
}

int kvspaceCp(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->cp(x->backend, src, dst, err, err_cap);
}

int kvspaceCpTree(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->cptree(x->backend, src, dst, err, err_cap);
}

int kvspaceCpList(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->cplist(x->backend, src, dst, err, err_cap);
}

int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->mkindex(x->backend, path, capacity, err, err_cap);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->mkindexext(x->backend, path, ext_path, err, err_cap);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->rmindexext(x->backend, path, err, err_cap);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    kvspace_handle *x = H(h);
    return x->vt->clear(x->backend, err, err_cap);
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                 uint64_t tick_ns, uint8_t **out, uint32_t *out_len) {
    kvspace_handle *x = H(h);
    return x->vt->watch(x->backend, key, target, target_len, tick_ns, out, out_len);
}

/* ── P2 token（后端缺符号 → -2，调用方回退）────────────────────── */
#define P2VT(field) (H(h)->vt->field)
int kvspaceViewSweep(void *h) {
    if (!P2VT(viewsweep)) return -2;
    return P2VT(viewsweep)(H(h)->backend);
}
int kvspaceViewResolvePrefix(void *h, const char *path, kvspaceRef_t *ref) {
    if (!P2VT(viewresolveprefix)) return -2;
    return P2VT(viewresolveprefix)(H(h)->backend, path, ref);
}
int kvspaceViewResolve(void *h, const char *key, kvspaceViewToken_t *tok) {
    if (!P2VT(viewresolve)) return -2;
    return P2VT(viewresolve)(H(h)->backend, key, tok);
}
int kvspaceViewFromRef(void *h, const kvspaceRef_t *ref, const char *k,
                       kvspaceViewToken_t *tok) {
    if (!P2VT(viewfromref)) return -2;
    return P2VT(viewfromref)(H(h)->backend, ref, k, tok);
}
int kvspaceViewResolveFrom(void *h, const kvspaceRef_t *parent, const char *rel,
                           kvspaceViewToken_t *tok) {
    if (!P2VT(viewresolvefrom)) return -2;
    return P2VT(viewresolvefrom)(H(h)->backend, parent, rel, tok);
}
int kvspaceViewCheck(void *h, const kvspaceViewToken_t *tok) {
    if (!P2VT(viewcheck)) return -2;
    return P2VT(viewcheck)(H(h)->backend, tok);
}
int kvspaceViewWrite(void *h, kvspaceViewToken_t *tok, const uint8_t *d, uint32_t n) {
    if (!P2VT(viewwrite)) return -2;
    return P2VT(viewwrite)(H(h)->backend, tok, d, n);
}
int kvspaceViewDel(void *h, kvspaceViewToken_t *tok, const char *key) {
    if (!P2VT(viewdel)) return -2;
    return P2VT(viewdel)(H(h)->backend, tok, key);
}
int kvspaceViewRefresh(void *h, kvspaceViewToken_t *tok, const char *key) {
    if (!P2VT(viewrefresh)) return -2;
    return P2VT(viewrefresh)(H(h)->backend, tok, key);
}
int kvspaceViewAck(void *h, uint32_t *epoch) {
    if (!P2VT(viewack)) return -2;
    return P2VT(viewack)(H(h)->backend, epoch);
}
int kvspaceViewQuiesce(void *h, int on) {
    if (!P2VT(viewquiesce)) return -2;
    return P2VT(viewquiesce)(H(h)->backend, on);
}
