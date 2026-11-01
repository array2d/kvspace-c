#include "xvalue_head.h"

#include <stdlib.h>
#include <string.h>

/* V2 final layout (#571): fixed 8-byte header [len:u48 LE@0-5][storetype:u8@6][ptr:u8@7],
   body always at offset 8, self-delimited attr region at 8+len (carrying langtype).
   Single-key atomic write — no separate attr key. Empty langtype = empty attr. */

#define UINT48_MAX UINT64_C(0xFFFFFFFFFFFF)

/* Failure helper: clear *out per header contract. */
static int fail(uint8_t **out) {
    if (out)
        *out = NULL;
    return -1;
}

/* Single wire builder. Zero-fills body when body is NULL (Reserve placeholder);
   public entry points validate whether non-zero len requires non-NULL body. */
static int build(uint8_t storetype, uint8_t ptr, const uint8_t *body,
                 uint64_t len, const char *langtype, uint8_t **out,
                 uint64_t *out_len) {
    if (!out || !out_len)
        return fail(out);
    *out = NULL;
    if (storetype > KVSPACE_XVALUE_ST_EXT || ptr > KVSPACE_XVALUE_PTR_EXT)
        return -1;
    if (len > UINT48_MAX)
        return -1;
    uint64_t ltl = langtype ? strlen(langtype) : 0;
    uint64_t alen = 2 + (ltl ? 3 + ltl : 0); /* attr 总长（含自身 2 字节） */
    if (alen > UINT16_MAX)
        return -1;
    uint64_t n = (uint64_t)KVSPACE_XVALUE_HEAD_LEN + len + alen;
    if (n > SIZE_MAX)
        return -1;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)n);
    if (!p)
        return -1;
    kvspaceXvalueHead h = {.storetype = storetype, .ptr = ptr};
    kvspaceXvalueHeadSetLen(&h, len);
    memcpy(p, &h, sizeof h);
    if (len && body)
        memcpy(p + KVSPACE_XVALUE_HEAD_LEN, body, len);
    uint8_t *a = p + KVSPACE_XVALUE_HEAD_LEN + len;
    a[0] = (uint8_t)alen;
    a[1] = (uint8_t)(alen >> 8);
    if (ltl) {
        a[2] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
        a[3] = (uint8_t)ltl;
        a[4] = (uint8_t)(ltl >> 8);
        memcpy(a + 5, langtype, ltl);
    }
    *out = p;
    *out_len = n;
    return 0;
}

int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len) {
    return build(KVSPACE_XVALUE_ST_SHORT, KVSPACE_XVALUE_PTR_INLINE, NULL, 0,
                 NULL, out, out_len);
}

int kvspaceXvalueHeadNewScalar(uint8_t storetype, const uint8_t *raw,
                               uint32_t raw_len, const char *langtype,
                               uint8_t **out, uint64_t *out_len) {
    if (raw_len && !raw)
        return fail(out);
    return build(storetype, KVSPACE_XVALUE_PTR_INLINE, raw, raw_len, langtype,
                 out, out_len);
}

int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              uint64_t cap, const char *langtype,
                              uint8_t **out, uint64_t *out_len) {
    if (cap < data_len || (data_len && !data))
        return fail(out);
    return build(KVSPACE_XVALUE_ST_SLACK, KVSPACE_XVALUE_PTR_INLINE, data,
                 data_len, langtype, out, out_len);
}

int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len) {
    /* Shape lives in leading [dims] of langtype; body byte length is raw_len. */
    if (!dims || ndim == 0 || ndim > 64 || (raw_len && !raw))
        return fail(out);
    return build(KVSPACE_XVALUE_ST_LARGE, KVSPACE_XVALUE_PTR_INLINE, raw,
                 raw_len, langtype, out, out_len);
}

int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len) {
    if (!path_len || !path)
        return fail(out);
    return build(KVSPACE_XVALUE_ST_SLACK, KVSPACE_XVALUE_PTR_PTR, path, path_len,
                 langtype, out, out_len);
}

int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len) {
    if (!locator_len || !locator)
        return fail(out);
    return build(KVSPACE_XVALUE_ST_EXT, KVSPACE_XVALUE_PTR_EXT, locator,
                 locator_len, langtype, out, out_len);
}

int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             uint64_t cap, const char *langtype,
                             uint8_t **out, uint64_t *out_len) {
    return kvspaceXvalueHeadNewSlack(body, body_len, cap, langtype, out,
                                     out_len);
}

int kvspaceXvalueHeadReserve(uint8_t storetype, uint64_t body_len,
                             uint64_t cap, const char *langtype,
                             uint8_t **out, uint64_t *out_len) {
    /* 固定头无容量字段：预留 = 产 body_len 字节零 body 的占位（后由调用方填充），
       attr 区（含 langtype 记录）一并写入。 */
    if (cap < body_len)
        return fail(out);
    return build(storetype, KVSPACE_XVALUE_PTR_INLINE, NULL, body_len,
                 langtype, out, out_len);
}
