#include "xvalue.h"
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 对齐 kvspace-durable 的 kindexp TLV 编解码。 */

static void wr_u8(uint8_t *dst, uint8_t v)  { dst[0] = v; }
static void wr_u16(uint8_t *dst, uint16_t v) { dst[0]=(uint8_t)v; dst[1]=(uint8_t)(v>>8); }
static void wr_u32(uint8_t *dst, uint32_t v) { for(int i=0;i<4;i++) dst[i]=(uint8_t)(v>>(i*8)); }
static void wr_u64(uint8_t *dst, uint64_t v) { for(int i=0;i<8;i++) dst[i]=(uint8_t)(v>>(i*8)); }
static void wr_i8(uint8_t *dst, int8_t v)   { wr_u8(dst, (uint8_t)v); }
static void wr_i16(uint8_t *dst, int16_t v)  { wr_u16(dst, (uint16_t)v); }
static void wr_i32(uint8_t *dst, int32_t v)  { wr_u32(dst, (uint32_t)v); }
static void wr_i64(uint8_t *dst, int64_t v)  { wr_u64(dst, (uint64_t)v); }

static uint32_t rd_u32(const uint8_t *r) {
    return (uint32_t)r[0] | ((uint32_t)r[1]<<8) | ((uint32_t)r[2]<<16) | ((uint32_t)r[3]<<24);
}

static int32_t header_array_len(int32_t ndim, const int32_t *dims) {
    if (ndim <= 0) return 1;
    int32_t n = 1;
    for (int i = 0; i < ndim; i++) n *= dims[i];
    return n;
}

int32_t kvspaceXvalueHeadLen(const xvalue_head_t *h) {
    return 1 + h->kindexprlen + 1 + 4 + 4;
}

static int32_t build_kindexpr(char *buf, int32_t cap, const char *kind, int32_t ref,
                              const int32_t *dims, int32_t ndim) {
    int32_t o = 0;
    if (ref == 1) buf[o++] = '*';
    else if (ref == 2) buf[o++] = '@';
    if (ndim > 0) {
        buf[o++] = '[';
        for (int i = 0; i < ndim; i++) {
            if (i > 0) buf[o++] = ',';
            o += snprintf(buf + o, (size_t)(cap - o), "%d", dims[i]);
        }
        buf[o++] = ']';
    }
    int32_t kl = (int32_t)strlen(kind);
    memcpy(buf + o, kind, (size_t)kl);
    return o + kl;
}

static int32_t encode_head(const char *kind, int32_t ref, int32_t ro, uint32_t vid,
                           const int32_t *dims, int32_t ndim,
                           const uint8_t *raw, int32_t raw_len, uint8_t **out) {
    char kx[256];
    int32_t kxl = build_kindexpr(kx, (int32_t)sizeof kx, kind, ref, dims, ndim);
    int32_t slot = kxl + 1;
    int32_t total = 1 + slot + 1 + 4 + 4 + raw_len;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return -1;
    buf[0] = (uint8_t)slot;
    memcpy(buf + 1, kx, (size_t)kxl);
    buf[1 + kxl] = 0; /* NUL（槽内 padding） */
    int32_t o = 1 + slot;
    buf[o] = (uint8_t)(ro ? 1 : 0);
    wr_u32(buf + o + 1, vid);
    wr_u32(buf + o + 5, (uint32_t)raw_len);
    if (raw_len > 0 && raw) memcpy(buf + o + 9, raw, (size_t)raw_len);
    *out = buf;
    return total;
}

int32_t kvspaceXvalueEncode(const char *kind, const uint8_t *raw, int32_t raw_len,
                      const int32_t *dims, int32_t ndim, uint8_t **out) {
    if (!out || !kind) return -1;
    if (ndim < 0) ndim = 0;
    if (ndim > X_MAX_NDIM) return -1;
    if (raw_len < 0) raw_len = 0;
    return encode_head(kind, 0, 0, 0, dims, ndim, raw, raw_len, out);
}

int32_t kvspaceXvalueEncodePtr(const char *kind, const uint8_t *raw, int32_t raw_len,
                          const int32_t *dims, int32_t ndim, uint8_t **out) {
    if (!out || !kind) return -1;
    if (ndim < 0) ndim = 0;
    if (ndim > X_MAX_NDIM) return -1;
    if (raw_len < 0) raw_len = 0;
    return encode_head(kind, 1, 0, 0, dims, ndim, raw, raw_len, out);
}

int32_t kvspaceXvalueEncodeMode(const char *kind, const uint8_t *raw, int32_t raw_len,
                          const int32_t *dims, int32_t ndim, int32_t ref, int32_t ro, uint32_t vid,
                          uint8_t **out) {
    if (!out || !kind) return -1;
    if (ndim < 0) ndim = 0;
    if (ndim > X_MAX_NDIM) return -1;
    if (raw_len < 0) raw_len = 0;
    return encode_head(kind, ref, ro, vid, dims, ndim, raw, raw_len, out);
}

/* 标量/一维便捷编码（内部）：array_len → dims。char/* 恒一维（含空串/单字符）；
 * 其余标量(≤1)=0 维、多元素=1 维。公开的 kvspaceXvalueEncode 只认 dims/ndim。 */
static int32_t al_to_dims(const char *kind, int32_t array_len, int32_t *dims) {
    if (strncmp(kind, "char/", 5) == 0) { dims[0] = array_len < 0 ? 0 : array_len; return 1; }
    if (array_len > 1) { dims[0] = array_len; return 1; }
    return 0;
}

static int32_t encode_al(const char *kind, const uint8_t *raw, int32_t raw_len,
                         int32_t array_len, uint8_t **out) {
    int32_t dims[1]; int32_t nd = al_to_dims(kind, array_len, dims);
    return kvspaceXvalueEncode(kind, raw, raw_len, dims, nd, out);
}

static int32_t encode_al_ptr(const char *kind, const uint8_t *raw, int32_t raw_len,
                             int32_t array_len, uint8_t **out) {
    if (array_len <= 0) array_len = 1;
    int32_t dims[1]; int32_t nd = al_to_dims(kind, array_len, dims);
    return kvspaceXvalueEncodePtr(kind, raw, raw_len, dims, nd, out);
}

xvalue_head_t kvspaceXvalueDecodeHead(const uint8_t *data, int32_t data_len) {
    xvalue_head_t h = {0};
    if (!data || data_len < 1) return h;
    int32_t slot = (int32_t)data[0];
    int32_t o = 1 + slot;
    if (data_len < o + 9) return h;
    const uint8_t *kx = data + 1;
    int32_t kxl = 0;
    while (kxl < slot && kx[kxl] != 0) kxl++;
    int32_t i = 0;
    if (kxl > 0 && kx[0] == '*') { h.ref = 1; i = 1; }
    else if (kxl > 0 && kx[0] == '@') { h.ref = 2; i = 1; }
    if (i < kxl && kx[i] == '[') {
        i++;
        while (i < kxl && kx[i] != ']' && h.ndim < X_MAX_NDIM) {
            int32_t d = 0;
            while (i < kxl && kx[i] >= '0' && kx[i] <= '9') { d = d * 10 + (kx[i] - '0'); i++; }
            h.dims[h.ndim++] = d;
            if (i < kxl && kx[i] == ',') i++;
        }
        if (i < kxl && kx[i] == ']') i++;
    }
    h.kind = (const char *)(kx + i);
    h.kind_len = kxl - i;
    h.kindexpr = (const char *)kx;
    h.kindexpr_len = kxl;
    h.kindexprlen = slot;
    h.ro = data[o] & 0x01;
    h.vid = rd_u32(data + o + 1);
    h.raw_len = (int32_t)rd_u32(data + o + 5);
    if (data_len < o + 9 + h.raw_len) return h;
    h.raw = data + o + 9;
    h.array_len = header_array_len(h.ndim, h.dims);
    return h;
}

#define DEF_NEW_ARRAY(name, kind, T, elem_sz, wr_fn)                      \
    int32_t kvspaceXvalueNew##name(const T *vals, int32_t count, uint8_t **out) { \
        if (!vals || count <= 0) return -1;                                  \
        int32_t raw_len = count * elem_sz;                                   \
        uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);                   \
        if (!raw) return -1;                                                 \
        for (int32_t i = 0; i < count; i++) wr_fn(raw + i * elem_sz, vals[i]); \
        int32_t r = encode_al(kind, raw, raw_len, count, out);               \
        free(raw);                                                           \
        return r;                                                            \
    }

DEF_NEW_ARRAY(Bool,    KVSPACE_KIND_BOOL,    bool,    1,  wr_u8)
DEF_NEW_ARRAY(Int8,    KVSPACE_KIND_INT8,    int8_t,  1,  wr_i8)
DEF_NEW_ARRAY(Int16,   KVSPACE_KIND_INT16,   int16_t, 2,  wr_i16)
DEF_NEW_ARRAY(Int32,   KVSPACE_KIND_INT32,   int32_t, 4,  wr_i32)
DEF_NEW_ARRAY(Int64,   KVSPACE_KIND_INT64,   int64_t, 8,  wr_i64)
DEF_NEW_ARRAY(Uint8,   KVSPACE_KIND_UINT8,   uint8_t, 1,  wr_u8)
DEF_NEW_ARRAY(Uint16,  KVSPACE_KIND_UINT16,  uint16_t,2,  wr_u16)
DEF_NEW_ARRAY(Uint32,  KVSPACE_KIND_UINT32,  uint32_t,4,  wr_u32)
DEF_NEW_ARRAY(Uint64,  KVSPACE_KIND_UINT64,  uint64_t,8,  wr_u64)

int32_t kvspaceXvalueNewFloat32(const float *vals, int32_t count, uint8_t **out) {
    if (!vals || count <= 0) return -1;
    int32_t raw_len = count * 4;
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);
    if (!raw) return -1;
    for (int32_t i = 0; i < count; i++) {
        union { float f; uint32_t u; } c = {vals[i]};
        wr_u32(raw + i * 4, c.u);
    }
    int32_t r = encode_al(KVSPACE_KIND_FLOAT32, raw, raw_len, count, out);
    free(raw);
    return r;
}

int32_t kvspaceXvalueNewFloat64(const double *vals, int32_t count, uint8_t **out) {
    if (!vals || count <= 0) return -1;
    int32_t raw_len = count * 8;
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);
    if (!raw) return -1;
    for (int32_t i = 0; i < count; i++) {
        union { double f; uint64_t u; } c = {vals[i]};
        wr_u64(raw + i * 8, c.u);
    }
    int32_t r = encode_al(KVSPACE_KIND_FLOAT64, raw, raw_len, count, out);
    free(raw);
    return r;
}

/* ── char/utf32：UTF-8 → UTF-32 LE ────────────────────────────────── */

static uint32_t utf8_next(const uint8_t *s, int32_t len, int32_t *i) {
    uint32_t cp = s[*i];
    if (cp < 0x80) { (*i)++; return cp; }
    int n;
    if ((cp & 0xE0) == 0xC0)      { n = 1; cp &= 0x1F; }
    else if ((cp & 0xF0) == 0xE0) { n = 2; cp &= 0x0F; }
    else if ((cp & 0xF8) == 0xF0) { n = 3; cp &= 0x07; }
    else { (*i)++; return 0xFFFD; }
    (*i)++;
    for (int j = 0; j < n && *i < len; j++, (*i)++) cp = (cp << 6) | (s[*i] & 0x3F);
    return cp;
}

int32_t kvspaceXvalueNewChar(const char *s, uint8_t **out) {
    if (!s || !*s) return encode_al(KVSPACE_KIND_CHAR, NULL, 0, 0, out);
    int32_t slen = (int32_t)strlen(s);
    uint8_t *raw = (uint8_t *)malloc((size_t)slen * 4);
    if (!raw) return -1;
    int32_t n = 0, i = 0;
    while (i < slen) {
        uint32_t cp = utf8_next((const uint8_t *)s, slen, &i);
        wr_u32(raw + n * 4, cp);
        n++;
    }
    int32_t r = encode_al(KVSPACE_KIND_CHAR, raw, n * 4, n, out);
    free(raw);
    return r;
}

int32_t kvspaceXvalueNewCharUtf8(const char *s, uint8_t **out) {
    if (!s) s = "";
    int32_t slen = (int32_t)strlen(s);
    return encode_al(KVSPACE_KIND_CHAR_UTF8, (const uint8_t *)s, slen, slen, out);
}

int32_t kvspaceXvalueNewCharAscii(const char *s, uint8_t **out) {
    if (!s) s = "";
    int32_t slen = (int32_t)strlen(s);
    return encode_al(KVSPACE_KIND_CHAR_ASCII, (const uint8_t *)s, slen, slen, out);
}

int32_t kvspaceXvalueAtChar(const xvalue_head_t *h, int32_t idx) {
    if (!h || !h->raw || idx < 0 || idx >= h->array_len) return 0;
    return (int32_t)rd_u32(h->raw + idx * 4);
}

/* ── index / ptr / extindex ──────────────────────────────────────── */

int32_t kvspaceXvalueNewIndex(const char **children, int32_t count, uint8_t **out) {
    size_t total = 4; /* [4B count LE] */
    for (int i = 0; i < count; i++) total += (children[i] ? strlen(children[i]) : 0);
    if (count > 0) total += (size_t)(count - 1);
    if (total > (size_t)INT32_MAX) return -1;
    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) return -1;
    wr_u32(raw, (uint32_t)count);
    size_t pos = 4;
    for (int i = 0; i < count; i++) {
        if (!children[i]) continue;
        size_t len = strlen(children[i]);
        memcpy(raw + pos, children[i], len);
        pos += len;
        if (i < count - 1) raw[pos++] = '\n';
    }
    int32_t r = encode_al(KVSPACE_KIND_INDEX, raw, (int32_t)pos, 1, out);
    free(raw);
    return r;
}

int32_t kvspaceXvalueNewMap(const char **children, int32_t count,
                            const int32_t *dims, int32_t ndim, uint8_t **out) {
    if (ndim <= 0 || ndim > X_MAX_NDIM)
        return -1;
    size_t total = 4; /* [4B count LE] */
    for (int i = 0; i < count; i++) total += (children[i] ? strlen(children[i]) : 0);
    if (count > 0) total += (size_t)(count - 1);
    if (total > (size_t)INT32_MAX) return -1;
    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) return -1;
    wr_u32(raw, (uint32_t)count);
    size_t pos = 4;
    for (int i = 0; i < count; i++) {
        if (!children[i]) continue;
        size_t len = strlen(children[i]);
        memcpy(raw + pos, children[i], len);
        pos += len;
        if (i < count - 1) raw[pos++] = '\n';
    }
    int32_t r = kvspaceXvalueEncode(KVSPACE_KIND_MAP, raw, (int32_t)pos, dims, ndim, out);
    free(raw);
    return r;
}

int32_t kvspaceXvalueNewPtr(const char *kind, const char *target, int32_t array_len, uint8_t **out) {
    if (!target) return -1;
    return encode_al_ptr(kind, (const uint8_t *)target, (int32_t)strlen(target), array_len, out);
}

#define EXT_PREFIX "…"

int32_t kvspaceXvalueNewExtindex(const char *extpath, const char **children, int32_t count, uint8_t **out) {
    if (!extpath) return -1;
    size_t plen = strlen(EXT_PREFIX) + strlen(extpath);
    size_t total = 4 + plen; /* [4B count LE] + extpath 段 */
    for (int i = 0; i < count; i++) total += (children[i] ? strlen(children[i]) : 0);
    if (count > 0) total += (size_t)count;
    if (total > (size_t)INT32_MAX) return -1;
    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) return -1;
    wr_u32(raw, (uint32_t)count);
    size_t pos = 4;
    memcpy(raw + pos, EXT_PREFIX, strlen(EXT_PREFIX)); pos += strlen(EXT_PREFIX);
    memcpy(raw + pos, extpath, strlen(extpath)); pos += strlen(extpath);
    for (int i = 0; i < count; i++) {
        raw[pos++] = '\n';
        if (!children[i]) continue;
        size_t len = strlen(children[i]);
        memcpy(raw + pos, children[i], len);
        pos += len;
    }
    int32_t r = encode_al(KVSPACE_KIND_EXT_INDEX, raw, (int32_t)pos, 1, out);
    free(raw);
    return r;
}
