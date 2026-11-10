/* KVSpace SHM ABI. */

/* macOS 需 _DARWIN_C_SOURCE 才暴露 usleep 等完整 Darwin API；_POSIX_C_SOURCE ≥200112
 * 才在 macOS 暴露 C99 的 snprintf（Linux 走 glibc 的 _DEFAULT_SOURCE）。 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE
#endif
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include "kvspace_shm.h"
#include <kvspace/xvalue_head.h>
#include "xvalue_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <limits.h>

#define SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

static int parse_shm_path(const char *dsn, char *out, size_t osz) {
    if (!dsn)
        return -1;
    const char *sep = strstr(dsn, "://");
    /* Scheme must be exactly "shm" (prefix matching would accept "s://" or "://"). */
    if (!sep || sep - dsn != 3 || strncmp(dsn, "shm", 3) != 0)
        return -1;
    int n = snprintf(out, osz, "%s", sep + 3);
    return n > 0 && (size_t)n < osz ? 0 : -1;  /* reject empty or truncated path */
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
    if (kvspaceShmSetPart((kvspace_t *)h, key, offset, buf, buf_len) != 0) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: set-part out of range at %s",
                     key ? key : "");
        return 1;
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

/* 新位置写：按 (ref, body_len, body_cap, langtype) 分配 box、写固定头 +
 * langtype 尾部（整体单写），返回 body 偏移指针。
 *
 * ── `storetype` 参数还在，但**本期不携带信息** ──────────────────────
 *
 * 签名必须与公开头逐参数对齐：前端 `frontend.c` 用 `dlsym` 按名字加载这个函数，
 * 少一个参数就是调用约定错位（未定义行为，编译不报）。
 *
 * 新布局没有 storetype 位：`body_cap != body_len` 只决定 effective_len 写进
 * attr tag 2（容量形态），引用性由 `ref` 决定（构造器给 langtype 加 `*`/`@`
 * 前缀）。所以本函数**不读**该参数、
 * 不校验它的取值、也不往下传——**它就是一个本期不再携带信息的参数**。
 * 加一条 `storetype <= …` 的检查只会让它看起来还有语义，并把「传了奇怪值」
 * 变成一条新的失败路径。
 *
 * `KVSPACE_STORETYPE_*` 常量的处置与签名删减是同一笔欠账，本期不动。 */
int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref,
                         uint8_t storetype, uint32_t body_len, uint64_t body_cap,
                         const char *langtype,
                         uint8_t **body, char *err, uint32_t err_cap) {
    (void)storetype; /* 见上：本期不携带信息，不分支、不校验、不传。 */
    if (kvspaceShmWriteNewPlace((kvspace_t *)h, key, ref,
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
    return kvspaceShmListAt((kvspace_t *)h, prefix, expand_ext != 0, resolve,
                            idx, buf, buf_cap, out_len);
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err,
               uint32_t err_cap) {
    if (!h || (!keys && nkeys > 0)) {
        if (err && err_cap)
            snprintf(err, err_cap, "kvspace: invalid arguments to del");
        return 1;
    }
    for (uint32_t i = 0; i < nkeys; i++) {
        if (!keys[i]) {
            if (err && err_cap)
                snprintf(err, err_cap, "kvspace: null key in del at index %u", i);
            return 1;
        }
        /* Missing keys and >=1024-byte keys are idempotent no-ops in KV del. */
        kvspaceShmDel((kvspace_t *)h, keys[i]);
    }
    return 0;
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    int rc = kvspaceShmDeltree((kvspace_t *)h, prefix);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: deltree failed at %s",
                 prefix ? prefix : "(null)");
    return rc;
}

int kvspaceCp(void *h, const char *src, const char *dst, char *err,
              uint32_t err_cap) {
    int rc = kvspaceShmCp((kvspace_t *)h, src, dst);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: cp failed from %s to %s",
                 src ? src : "(null)", dst ? dst : "(null)");
    return rc;
}

int kvspaceCpTree(void *h, const char *src, const char *dst, char *err,
                   uint32_t err_cap) {
    int rc = kvspaceShmCptree((kvspace_t *)h, src, dst);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: cptree failed from %s to %s",
                 src ? src : "(null)", dst ? dst : "(null)");
    return rc;
}

int kvspaceCpList(void *h, const char *src, const char *dst, char *err,
                   uint32_t err_cap) {
    int rc = kvspaceShmCplist((kvspace_t *)h, src, dst);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: cplist failed from %s to %s",
                 src ? src : "(null)", dst ? dst : "(null)");
    return rc;
}

int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err,
                   uint32_t err_cap) {
    int rc = kvspaceShmMkindex((kvspace_t *)h, path, capacity);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: mkindex failed at %s",
                 path ? path : "(null)");
    return rc;
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path,
                      char *err, uint32_t err_cap) {
    int rc = kvspaceShmExtindex((kvspace_t *)h, path, ext_path, err, err_cap);
    if (rc != 0 && err && err_cap && err[0] == '\0')
        snprintf(err, err_cap, "kvspace: mkindex-ext failed at %s -> %s",
                 path ? path : "(null)", ext_path ? ext_path : "(null)");
    return rc;
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    int rc = kvspaceShmDelextindex((kvspace_t *)h, path);
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: rmindex-ext failed at %s",
                 path ? path : "(null)");
    return rc;
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    int rc = kvspaceShmDeltree((kvspace_t *)h, "/");
    if (rc != 0 && err && err_cap)
        snprintf(err, err_cap, "kvspace: clear failed");
    return rc;
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
    int rc = kvspaceXvalueHeadNewSlack(bytes, len, langtype, &value, &total);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceNewBool(uint8_t v, const char *langtype,
                   uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = kvspaceXvalueHeadNewScalar(&b, 1, langtype,
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
    int rc = kvspaceXvalueHeadNewScalar(raw, 8, langtype,
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
    int rc = kvspaceXvalueHeadNewScalar(raw, 8, langtype,
                                        &value, &total);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

static int wire_is_none(const uint8_t *d, uint32_t len) {
    if (!d || len == 0)
        return 1;
    if (len < KVSPACE_XVALUE_HEAD_LEN)
        return 0;
    const kvspaceXvalueHead *xh = (const kvspaceXvalueHead *)d;
    /* None 的判据：**空 body + 无 langtype 记录（或 langtype 恰是 "None"）**。
       旧模型用 `storetype == ST_SHORT && ptr == INLINE` 表达「非引用且在 place」，
       新模型里「是不是引用」看 langtype 前缀——空 langtype 即非引用，所以
       长度 0 加下面的 langtype 检查已经足够，无需 storetype。 */
    if (kvspace_c_reference_of(d, len) != KVSPACE_XVALUE_REFERENCE_INLINE ||
        kvspaceXvalueHeadLen(xh) != 0)
        return 0;
    const uint8_t *lt = NULL;
    uint16_t ltn = kvspaceXvalueHeadLangtypeBounded(d, len, &lt);
    return ltn == 0 || (ltn == 4 && memcmp(lt, "None", 4) == 0);
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target,
                 uint32_t target_len, uint64_t tick_ns, uint8_t **out,
                 uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    *out = NULL;
    *out_len = 0;
    const int target_is_none = wire_is_none(target, target_len);
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (target_is_none) {
            if (!d || len <= 0 || wire_is_none(d, (uint32_t)len)) {
                *out = d;
                *out_len = (d && len > 0) ? (uint32_t)len : 0;
                return 0;
            }
        } else if (d && (uint32_t)len == target_len &&
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
