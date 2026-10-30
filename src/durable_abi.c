/* KVSpace SHM ABI. */

/* macOS 需 _DARWIN_C_SOURCE 才暴露 usleep 等完整 Darwin API；_POSIX_C_SOURCE ≥200112
 * 才在 macOS 暴露 C99 的 snprintf（Linux 走 glibc 的 _DEFAULT_SOURCE）。 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>

#define SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

/* Mirrors the public ABI. */
typedef struct {
    uint16_t headlen;
    uint8_t ref;
    uint8_t storetype;
    int32_t body_len;
    int32_t ndim;
    int32_t dims[8];
    char langtype[256];
    int32_t langtype_len;
    int32_t body_offset;
    uint64_t body_cap;
} kvspaceHead_t;

/* 固定 8 字节 head → 元数据（langtype 在 body 尾 @8+len，见 kvspaceGetHead）。 */
static int from_wire_head(const uint8_t *d, uint32_t total, kvspaceHead_t *out) {
    if (!out || !d || total < KVSPACE_XVALUE_HEAD_LEN)
        return 1;
    memset(out, 0, sizeof *out);
    kvspaceXvalueHead h;
    memcpy(&h, d, sizeof h);
    uint64_t l = kvspaceXvalueHeadLen(&h);
    if (h.storetype > KVSPACE_XVALUE_ST_EXT || h.ptr > KVSPACE_XVALUE_PTR_EXT ||
        (uint64_t)KVSPACE_XVALUE_HEAD_LEN + l > total)
        return 1;
    out->headlen = KVSPACE_XVALUE_HEAD_LEN;
    out->ref = h.ptr == KVSPACE_XVALUE_PTR_PTR ? 1 :
               h.ptr == KVSPACE_XVALUE_PTR_EXT ? 2 : 0;
    out->storetype = h.storetype;
    out->body_len = l > INT32_MAX ? INT32_MAX : (int32_t)l;
    out->body_offset = KVSPACE_XVALUE_HEAD_LEN;
    out->body_cap = l;
    return 0;
}

static int parse_shm_path(const char *dsn, char *out, size_t osz) {
    const char *sep = strstr(dsn, "://");
    if (!sep || strncmp(dsn, "shm", (size_t)(sep - dsn)) != 0)
        return -1;
    snprintf(out, osz, "%s", sep + 3);
    return 0;
}

void *kvspaceConnect(const char *dsn) {
    char path[1024];
    if (parse_shm_path(dsn, path, sizeof path) != 0)
        return NULL;
    return (void *)kvspaceShmOpen(path, SHM_DEFAULT_SIZE);
}

void kvspaceClose(void *h) {
    if (h)
        kvspaceShmClose((kvspace_t *)h);
}

/* 借用读：*out 指向 SHM 常驻映射（生命周期同该槽），调用方不得 free。 */
int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out,
               uint32_t *out_len) {
    int32_t len = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, resolve, &len);
    if (!d || len <= 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    *out = d;
    *out_len = (uint32_t)len;
    return 0;
}

int kvspaceSetValue(void *h, const char *key, const uint8_t *value,
                    uint32_t value_len, char *err, uint32_t err_cap) {
    int rc = value_len > INT32_MAX ? -2 :
             kvspaceShmSetValue((kvspace_t *)h, key, value,
                                (int32_t)value_len);
    if (rc == -2) {
        if (err && err_cap)
            snprintf(err, err_cap, "invalid XValue");
        return 1;
    }
    if (rc != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-value failed at %s", key ? key : "");
        return 1;
    }
    return 0;
}

int kvspaceSetValueByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                         const uint8_t *value, uint32_t value_len,
                         char *err, uint32_t err_cap) {
    int rc = value_len > INT32_MAX
                 ? -2
                 : kvspaceShmSetValueByRef((kvspace_t *)h, ref, key_fallback,
                                           value, (int32_t)value_len);
    if (rc == -2) {
        if (err && err_cap)
            snprintf(err, err_cap, "invalid XValue");
        return 1;
    }
    if (rc != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-value-by-ref failed at %s",
                     key_fallback ? key_fallback : "");
        return 1;
    }
    return 0;
}

int kvspaceResolveRef(void *h, const char *key, kvspaceRef_t *ref) {
    return kvspaceShmResolveRef((kvspace_t *)h, key, ref);
}

int kvspaceGetByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                    uint8_t **out, uint32_t *out_len) {
    int32_t len = 0;
    uint8_t *d = kvspaceShmGetByRef((kvspace_t *)h, ref, key_fallback, &len);
    if (!d || len <= 0) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    *out = d;
    *out_len = (uint32_t)len;
    return 0;
}

int kvspaceXvalueBodyView(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                    kvspaceXvalueBodyView_t *out) {
    return kvspaceShmXvalueBodyView((kvspace_t *)h, ref, key_fallback, out);
}

int kvspaceSetPartByRef(void *h, kvspaceRef_t *ref, const char *key_fallback,
                        uint32_t offset, const uint8_t *buf, uint32_t buf_len,
                        char *err, uint32_t err_cap) {
    if (kvspaceShmSetPartByRef((kvspace_t *)h, ref, key_fallback, offset, buf,
                               buf_len) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-part-by-ref failed");
        return 1;
    }
    return 0;
}

/* 定位读：*out 指向 SHM 常驻映射内 [offset, offset+len)（借用，不得 free）。
 * 越界/空 → *out=NULL、out_len=0。 */
int kvspaceGetPart(void *h, const char *key, uint32_t offset, uint32_t len,
                    uint8_t **out, uint32_t *out_len) {
    int32_t total = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &total);
    if (!d || total <= 0 || offset >= (uint32_t)total) {
        *out = NULL;
        *out_len = 0;
        return 0;
    }
    uint32_t avail = (uint32_t)total - offset;
    *out = d + offset;
    *out_len = len < avail ? len : avail;
    return 0;
}

/* 定位写：就地 memcpy buf 到 SHM 映射内 [offset, offset+buf_len)（key 须已存在、不改结构）。 */
int kvspaceSetPart(void *h, const char *key, uint32_t offset,
                    const uint8_t *buf, uint32_t buf_len, char *err,
                    uint32_t err_cap) {
    int32_t total = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &total);
    if (!d || total < 0 || offset + buf_len > (uint32_t)total) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-part out of range at %s", key);
        return 1;
    }
    memcpy(d + offset, buf, buf_len);
    return 0;
}

/* 读 head：取值前缀的三正交轴 head（不取 body）。空/不存在 → 返回 1。 */
int kvspaceGetHead(void *h, const char *key, kvspaceHead_t *out) {
    int32_t total = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &total);
    if (!d || total <= 0)
        return 1;
    if (from_wire_head(d, (uint32_t)total, out) != 0)
        return 1;
    /* langtype 在 attr 区 tag=1 记录（@8+len 自描述）；无记录 = 无类型。 */
    uint64_t blen = (uint64_t)out->body_len;
    const uint8_t *lt = NULL;
    uint16_t ltl = kvspaceXvalueHeadLangtype(d, blen, &lt);
    if (lt && ltl > 0) {
        size_t take = (size_t)ltl < (uint64_t)sizeof out->langtype - 1
                          ? (size_t)ltl
                          : (size_t)sizeof out->langtype - 1;
        memcpy(out->langtype, lt, take);
        out->langtype[take] = 0;
        out->langtype_len = (int32_t)take;
    } else {
        out->langtype_len = 0;
    }
    return 0;
}

/* 就地写：返回原 box body 偏移指针；前置条件不满足 → 非 0 + err。 */
int kvspaceWriteInPlace(void *h, const char *key, int resolve,
                        uint32_t body_len, uint8_t **body, char *err,
                        uint32_t err_cap) {
    if (kvspaceShmWriteInPlace((kvspace_t *)h, key, resolve, (int32_t)body_len,
                               body) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: write-in-place rejected at %s", key);
        return 1;
    }
    return 0;
}

/* 新位置写：按 (ref, storetype, body_len, body_cap, langtype) 分配 box、写固定头 +
 * langtype 尾部（整体单写），返回 body 偏移指针。 */
int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref,
                         uint8_t storetype, uint32_t body_len, uint64_t body_cap,
                         const char *langtype,
                         uint8_t **body, char *err, uint32_t err_cap) {
    if (kvspaceShmWriteNewPlace((kvspace_t *)h, key, ref, storetype,
                                (int32_t)body_len, body_cap, langtype,
                                body) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: write-new-place failed at %s", key);
        return 1;
    }
    return 0;
}

/* 只返回前缀下子项计数，无缓冲、无需释放。 */
int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve,
                   int32_t *out_count) {
    return kvspaceShmListLen((kvspace_t *)h, prefix, expand_ext != 0, resolve,
                             out_count);
}

/* 索引取项：把前缀下第 idx 个直接子项名写进调用方自备缓冲 buf（容量
   buf_cap），*out_len 置该名长度（不含 NUL）。库侧零状态。idx 越界或缓冲不足 →
   返回非 0（缓冲不足时 *out_len 仍为所需长度，不静默截断）。配合 kvspaceListLen
   遍历。 */
int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve,
                  int32_t idx, uint8_t *buf, uint32_t buf_cap,
                  uint32_t *out_len) {
    *out_len = 0;
    char **names = NULL;
    int32_t count = 0;
    if (kvspaceShmList((kvspace_t *)h, prefix, expand_ext != 0, resolve, &names,
                       &count) != 0)
        return -1;
    int rc = -1;
    if (idx >= 0 && idx < count) {
        size_t l = strlen(names[idx]);
        *out_len = (uint32_t)l;
        if (buf && l <= buf_cap) {
            memcpy(buf, names[idx], l);
            rc = 0;
        }
    }
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
    return rc;
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err,
               uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    for (uint32_t i = 0; i < nkeys; i++)
        kvspaceShmDel((kvspace_t *)h, keys[i]);
    return 0;
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, prefix);
}

int kvspaceCp(void *h, const char *src, const char *dst, char *err,
              uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmCp((kvspace_t *)h, src, dst);
}

int kvspaceCpTree(void *h, const char *src, const char *dst, char *err,
                  uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmCptree((kvspace_t *)h, src, dst);
}

int kvspaceCpList(void *h, const char *src, const char *dst, char *err,
                  uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmCplist((kvspace_t *)h, src, dst);
}

int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err,
                   uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmMkindex((kvspace_t *)h, path, capacity);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path,
                      char *err, uint32_t err_cap) {
    return kvspaceShmExtindex((kvspace_t *)h, path, ext_path, err, err_cap);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmDelextindex((kvspace_t *)h, path);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    (void)err;
    (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, "/");
}

/* New* 统一收口：New* 构造 head+body（malloc），total 超 u32 上限即失败。 */
static int finish(uint8_t *value, uint64_t total, uint8_t **out, uint32_t *out_len) {
    if (!value || total > UINT32_MAX) {
        free(value);
        return 1;
    }
    *out = value;
    *out_len = (uint32_t)total;
    return 0;
}

int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len)
        return 1;
    /* `any` 不是类型名（空 kindexpr = 不约束）。 */
    if (strcmp(target_kindexpr, "any") == 0)
        return 1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewPtr((const uint8_t *)target,
                                (uint32_t)strlen(target), target_kindexpr,
                                &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, const char *langtype,
                   uint8_t **out, uint32_t *out_len) {
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = kvspaceXvalueHeadNewSlack(bytes, len, len, langtype, &value, &total);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceNewBool(uint8_t v, const char *langtype,
                   uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, &b, 1, langtype,
                                        &value, &total);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceNewInt64(int64_t v, const char *langtype,
                    uint8_t **out, uint32_t *out_len) {
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)((uint64_t)v >> (8 * i));
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, langtype,
                                        &value, &total);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceNewFloat64(double v, const char *langtype,
                      uint8_t **out, uint32_t *out_len) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)(bits >> (8 * i));
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, langtype,
                                        &value, &total);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target,
                 uint32_t target_len, uint64_t tick_ns, uint8_t **out,
                 uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    *out = NULL;
    *out_len = 0;
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (d && (uint32_t)len == target_len &&
            memcmp(d, target, target_len) == 0) {
            *out = d;
            *out_len = (uint32_t)len;
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &tn);
        uint64_t elapsed = (uint64_t)(tn.tv_sec - t0.tv_sec) * 1000000000ULL +
                           (uint64_t)(tn.tv_nsec - t0.tv_nsec);
        if (elapsed >= tick_ns)
            return 0;
        usleep(1000);
    }
}


/* ── P2 token ABI wrappers（durable 后端后续给 ENOTSUP）──────────── */
int kvspaceViewResolvePrefix(void *h, const char *path, kvspaceRef_t *ref) {
    return kvspaceShmResolvePrefix((kvspace_t *)h, path, ref) == 0 ? 0 : 1;
}

int kvspaceViewAck(void *h, uint32_t *epoch) {
    return kvspaceShmViewAck((kvspace_t *)h, epoch);
}

int kvspaceViewQuiesce(void *h, int on) {
    return kvspaceShmViewQuiesce((kvspace_t *)h, on);
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
