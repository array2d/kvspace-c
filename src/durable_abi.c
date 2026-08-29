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

/* 非法目录前缀（不是 / 且不以 / 或 · 结尾）→ 非 0，对齐 durable validate_dir。 */
static int bad_dir_prefix(const char *p) {
    if (!p || !p[0]) return 1;
    if (strcmp(p, "/") == 0) return 0;
    size_t l = strlen(p);
    if (p[l - 1] == '/') return 0;
    if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 && (unsigned char)p[l - 1] == 0xB7) return 0;
    return 1;
}

void *kvspaceConnect(const char *dsn) {
    char path[1024];
    if (parse_shm_path(dsn, path, sizeof path) != 0) return NULL;
    return (void *)kvspaceShmOpen(path, SHM_DEFAULT_SIZE);
}

void kvspaceClose(void *h) {
    if (h) kvspaceShmClose((kvspace_t *)h);
}

void kvspaceBytesFree(uint8_t *p, uint32_t len) {
    (void)len;
    free(p);
}

int kvspaceSet(void *h, const char *const *keys, const uint8_t *vals,
                const uint32_t *lens, uint32_t n, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        int rc = kvspaceShmSet((kvspace_t *)h, keys[i], vals + off, (int32_t)lens[i]);
        off += lens[i];
        if (rc != 0) {
            if (err && err_cap)
                snprintf(err, err_cap, "kvspace: set failed at key %s", keys[i]);
            return 1;
        }
    }
    return 0;
}

int kvspaceGet(void *h, const char *key, uint8_t **out, uint32_t *out_len) {
    int32_t len;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
    if (!d || len <= 0) { *out = NULL; *out_len = 0; return 0; }
    uint8_t *c = malloc((size_t)len);
    memcpy(c, d, (size_t)len);
    *out = c; *out_len = (uint32_t)len;
    return 0;
}


int kvspaceList(void *h, const char *prefix, int expand_ext, int resolve,
                 uint8_t **out, uint32_t *out_len) {
    char **names; int32_t count;
    if (kvspaceShmList((kvspace_t *)h, prefix, expand_ext, resolve, &names, &count) != 0) {
        *out = NULL; *out_len = 0; return 1;
    }
    size_t total = 0;
    for (int32_t i = 0; i < count; i++) total += strlen(names[i]) + 1;
    uint8_t *buf = total ? malloc(total) : NULL;
    size_t off = 0;
    for (int32_t i = 0; i < count; i++) {
        size_t l = strlen(names[i]);
        memcpy(buf + off, names[i], l);
        off += l;
        if (i < count - 1) buf[off++] = '\n';
    }
    for (int32_t i = 0; i < count; i++) free(names[i]);
    free(names);
    *out = buf; *out_len = (uint32_t)off;
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

int kvspaceTlvEncodePtr(const char *kind, const uint8_t *raw, uint32_t raw_len,
                           const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueEncodePtr(kind, raw, (int32_t)raw_len, dims, ndim, out);
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

int kvspaceNewPtr(const char *kind, const char *target, int32_t array_len,
                    uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewPtr(kind, target, array_len, out);
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

int kvspaceGetBatch(void *h, const char *prefix, const char *const *names,
                      uint32_t nnames, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return 1;
    *out = NULL;
    *out_len = 0;
    if (bad_dir_prefix(prefix)) return 1;
    if (!names || nnames == 0) return 0;
    size_t total = (size_t)nnames * 4;
    for (uint32_t i = 0; i < nnames; i++) {
        char key[2048];
        snprintf(key, sizeof key, "%s%s", prefix, names[i]);
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (d && len > 0) total += (size_t)len;
    }
    uint8_t *buf = malloc(total);
    if (!buf) return 1;
    size_t off = 0;
    for (uint32_t i = 0; i < nnames; i++) {
        char key[2048];
        snprintf(key, sizeof key, "%s%s", prefix, names[i]);
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (!d || len <= 0) len = 0;
        buf[off] = (uint8_t)(len & 0xFF);
        buf[off + 1] = (uint8_t)((len >> 8) & 0xFF);
        buf[off + 2] = (uint8_t)((len >> 16) & 0xFF);
        buf[off + 3] = (uint8_t)((len >> 24) & 0xFF);
        off += 4;
        if (len > 0) {
            memcpy(buf + off, d, (size_t)len);
            off += (size_t)len;
        }
    }
    *out = buf;
    *out_len = (uint32_t)off;
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
            uint8_t *c = malloc((size_t)len);
            memcpy(c, d, (size_t)len);
            *out = c;
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
