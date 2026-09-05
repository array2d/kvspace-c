/* durable_abi.c — 对齐 kvspace-durable 的 C ABI（kvspaceConnect 等 22 符号），
 * 让 kvlang 的 Rust layout 零改动对接 kvspace-c 的 SHM。内部复用 kvspaceShm* 与 kvspaceXvalue*。 */

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "kvspace_shm.h"
#include "xvalue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

/* 对齐 kvspace-durable 的 kvspaceHead_t（repr(C)）。kindexpr 为唯一类型真相。 */
typedef struct {
    uint8_t kindexpr[256];
    uint8_t ro;
    uint32_t vid;
    int32_t body_len;
    int32_t body_offset;
} kvspaceHead_t;

static int parse_shm_path(const char *dsn, char *out, size_t osz) {
    const char *sep = strstr(dsn, "://");
    if (!sep || strncmp(dsn, "shm", (size_t)(sep - dsn)) != 0) return -1;
    snprintf(out, osz, "%s", sep + 3);
    return 0;
}

void *kvspaceConnect(const char *dsn) {
    char path[1024];
    if (parse_shm_path(dsn, path, sizeof path) != 0) return NULL;
    return (void *)kvspaceShmOpen(path, SHM_DEFAULT_SIZE);
}

void kvspaceClose(void *h) {
    if (h) kvspaceShmClose((kvspace_t *)h);
}

/* 借用读：*out 指向 SHM 常驻映射（生命周期同该槽），调用方不得 free。 */
int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len) {
    int32_t len = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, resolve, &len);
    if (!d || len <= 0) { *out = NULL; *out_len = 0; return 0; }
    *out = d; *out_len = (uint32_t)len;
    return 0;
}

/* 就地写：返回原 box body 偏移指针；前置条件不满足 → 非 0 + err。 */
int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap) {
    if (kvspaceShmWriteInPlace((kvspace_t *)h, key, resolve, (int32_t)body_len, body) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "kvspace: write-in-place rejected at %s", key);
        return 1;
    }
    return 0;
}

/* 新位置写：按 (kindexpr, body_len) 分配 box、写 head，返回 body 偏移指针。 */
int kvspaceWriteNewPlace(void *h, const char *key, const char *kindexpr, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap) {
    if (kvspaceShmWriteNewPlace((kvspace_t *)h, key, kindexpr, (int32_t)body_len, body) != 0) {
        if (err && err_cap) snprintf(err, err_cap, "kvspace: write-new-place failed at %s", key);
        return 1;
    }
    return 0;
}

/* 只返回前缀下子项计数，无缓冲、无需释放。 */
int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count) {
    return kvspaceShmListLen((kvspace_t *)h, prefix, expand_ext != 0, resolve, out_count);
}

/* 借用枚举：*out 指向线程局部回收缓冲（\n 连接的直接子项名），生命周期至下次同线程 List，
   调用方不得 free。空目录 → *out=NULL、*out_len=0。 */
static __thread uint8_t *list_buf = NULL;
static __thread size_t list_cap = 0;

int kvspaceList(void *h, const char *prefix, int expand_ext, int resolve,
                uint8_t **out, uint32_t *out_len) {
    *out = NULL;
    *out_len = 0;
    char **names = NULL;
    int32_t count = 0;
    if (kvspaceShmList((kvspace_t *)h, prefix, expand_ext != 0, resolve, &names, &count) != 0)
        return -1;
    size_t need = 0;
    for (int32_t i = 0; i < count; i++)
        need += strlen(names[i]) + 1;
    if (need == 0) {
        for (int32_t i = 0; i < count; i++)
            free(names[i]);
        free(names);
        return 0;
    }
    if (need > list_cap) {
        uint8_t *nb = realloc(list_buf, need);
        if (!nb) {
            for (int32_t i = 0; i < count; i++)
                free(names[i]);
            free(names);
            return -1;
        }
        list_buf = nb;
        list_cap = need;
    }
    size_t o = 0;
    for (int32_t i = 0; i < count; i++) {
        size_t l = strlen(names[i]);
        if (i)
            list_buf[o++] = '\n';
        memcpy(list_buf + o, names[i], l);
        o += l;
        free(names[i]);
    }
    free(names);
    *out = list_buf;
    *out_len = (uint32_t)o;
    return 0;
}

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    for (uint32_t i = 0; i < nkeys; i++) kvspaceShmDel((kvspace_t *)h, keys[i]);
    return 0;
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, prefix);
}

int kvspaceCp(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmCp((kvspace_t *)h, src, dst);
}

int kvspaceCpTree(void *h, const char *src, const char *dst, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmCptree((kvspace_t *)h, src, dst);
}

int kvspaceMkindex(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmMkindex((kvspace_t *)h, path);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmExtindex((kvspace_t *)h, path, ext_path);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmDelextindex((kvspace_t *)h, path);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, "/");
}

int kvspaceDisconnect(void *h, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return 0;
}

int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                       const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueEncode(kind, raw, (int32_t)raw_len, dims, ndim, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                           const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                           uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueEncodeMode(kind, raw, (int32_t)raw_len, dims, ndim, ref, (int32_t)ro, vid, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out) return 1;
    xvalue_head_t h = kvspaceXvalueDecodeHead(data, (int32_t)data_len);
    memset(out, 0, sizeof(*out));
    int32_t kl = h.kindexpr_len;
    if (kl > 255) kl = 255;
    memcpy(out->kindexpr, h.kindexpr, (size_t)kl);
    out->kindexpr[kl] = 0;
    out->ro = (uint8_t)h.ro;
    out->vid = h.vid;
    out->body_len = h.raw_len;
    out->body_offset = kvspaceXvalueHeadLen(&h);
    return 0;
}

int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                    uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewPtr(target_kindexpr, target, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    int32_t d = (int32_t)len;
    int32_t n = kvspaceXvalueEncode(KVSPACE_KIND_CHAR_UTF8, bytes, (int32_t)len, &d, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    bool b = v != 0;
    int32_t n = kvspaceXvalueNewBool(&b, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewInt64(&v, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewFloat64(&v, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                  uint64_t tick_ns, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return 1;
    *out = NULL;
    *out_len = 0;
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (d && (uint32_t)len == target_len && memcmp(d, target, target_len) == 0) {
            *out = d;
            *out_len = (uint32_t)len;
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &tn);
        uint64_t elapsed = (uint64_t)(tn.tv_sec - t0.tv_sec) * 1000000000ULL +
                           (uint64_t)(tn.tv_nsec - t0.tv_nsec);
        if (elapsed >= tick_ns) return 0;
        usleep(1000);
    }
}
