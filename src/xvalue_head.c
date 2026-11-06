#include <kvspace/xvalue_head.h>

#include <stdlib.h>
#include <string.h>

/* Layout: fixed 16-byte header [bodylen:u64@0][attrslen:u32@8][layout@12][padding@13]
   [padding@14,16), body at offset 16, attr region right after the body with NO length
   prefix (attrslen is in the head — a second prefix would be a second truth).
   Single-key atomic write — no separate attr key. Empty langtype = empty attr.
   layout=1 时 bodylen 是容量：body 区按容量分配、有效长度写进 attr tag 2。 */

#define REALLEN_RECORD_LEN 11u /* [tag:u8][len:u16][reallen:u64] */

/* Failure helper: clear *out per header contract. */
static int fail(uint8_t **out) {
    if (out)
        *out = NULL;
    return -1;
}

/* Single wire builder. Zero-fills the body when body is NULL (Reserve
   placeholder); public entry points validate whether non-zero effective_len
   requires non-NULL body.
   langtype_prefix 只由 NewPtr/NewExt 传入 '*' 或 '@'，构造成 langtype 记录的前缀。 */
static int build(uint8_t layout, const uint8_t *body, uint64_t region_len,
                 uint64_t effective_len, char langtype_prefix,
                 const char *langtype, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return fail(out);
    *out = NULL;
    if (layout == KVSPACE_XVALUE_LAYOUT_LENGTH) {
        if (effective_len != region_len)
            return -1;
    } else if (layout == KVSPACE_XVALUE_LAYOUT_CAPACITY) {
        if (effective_len > region_len)
            return -1;
    } else {
        return -1;
    }
    const uint64_t ltl = langtype ? (uint64_t)strlen(langtype) : 0;
    const uint64_t record_len = ltl + (langtype_prefix ? 1u : 0u);
    if (record_len > UINT16_MAX)
        return -1;
    uint64_t alen = record_len ? 3 + record_len : 0;
    if (layout == KVSPACE_XVALUE_LAYOUT_CAPACITY)
        alen += REALLEN_RECORD_LEN;
    if (alen > UINT32_MAX)
        return -1;
    if (region_len > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN ||
        alen > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN - region_len)
        return -1;
    const uint64_t n = (uint64_t)KVSPACE_XVALUE_HEAD_LEN + region_len + alen;
    if (n > SIZE_MAX)
        return -1;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)n);
    if (!p)
        return -1;
    kvspaceXvalueHead h = {.layout = layout, .padding = {0u, 0u, 0u}};
    kvspaceXvalueHeadSetLen(&h, region_len);
    kvspaceXvalueHeadSetAttrsLen(&h, (uint32_t)alen);
    memcpy(p, &h, sizeof h);
    if (body && effective_len)
        memcpy(p + KVSPACE_XVALUE_HEAD_LEN, body, effective_len);
    uint8_t *a = p + KVSPACE_XVALUE_HEAD_LEN + region_len;
    if (record_len) {
        a[0] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
        a[1] = (uint8_t)record_len;
        a[2] = (uint8_t)(record_len >> 8);
        uint64_t at = 3;
        if (langtype_prefix)
            a[at++] = (uint8_t)langtype_prefix;
        if (ltl)
            memcpy(a + at, langtype, ltl);
        a += 3 + record_len;
    }
    if (layout == KVSPACE_XVALUE_LAYOUT_CAPACITY) {
        a[0] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
        a[1] = 8;
        a[2] = 0;
        for (int i = 0; i < 8; i++)
            a[3 + i] = (uint8_t)(effective_len >> (8 * i));
    }
    *out = p;
    *out_len = n;
    return 0;
}

int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len) {
    return build(KVSPACE_XVALUE_LAYOUT_LENGTH, NULL, 0, 0, 0, NULL, out,
                 out_len);
}

int kvspaceXvalueHeadNewScalar(const uint8_t *raw, uint32_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len) {
    if (raw_len && !raw)
        return fail(out);
    return build(KVSPACE_XVALUE_LAYOUT_LENGTH, raw, raw_len, raw_len, 0,
                 langtype, out, out_len);
}

int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              uint64_t cap, const char *langtype,
                              uint8_t **out, uint64_t *out_len) {
    if (cap < data_len || (data_len && !data))
        return fail(out);
    const uint8_t layout = cap > data_len ? KVSPACE_XVALUE_LAYOUT_CAPACITY
                                          : KVSPACE_XVALUE_LAYOUT_LENGTH;
    return build(layout, data, cap, data_len, 0, langtype, out, out_len);
}

int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len) {
    /* Shape lives in leading [dims] of langtype; body byte length is raw_len. */
    if (!dims || ndim == 0 || ndim > 64 || (raw_len && !raw))
        return fail(out);
    return build(KVSPACE_XVALUE_LAYOUT_LENGTH, raw, raw_len, raw_len, 0,
                 langtype, out, out_len);
}

/* path_len == 0 是合法的 None 指针：`*T` + 空 body。 */
int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len) {
    if (path_len && !path)
        return fail(out);
    if (langtype && (langtype[0] == '*' || langtype[0] == '@'))
        return fail(out);
    return build(KVSPACE_XVALUE_LAYOUT_LENGTH, path, path_len, path_len, '*',
                 langtype, out, out_len);
}

int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len) {
    if (!locator_len || !locator)
        return fail(out);
    if (langtype && (langtype[0] == '*' || langtype[0] == '@'))
        return fail(out);
    return build(KVSPACE_XVALUE_LAYOUT_LENGTH, locator, locator_len, locator_len,
                 '@', langtype, out, out_len);
}

int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             uint64_t cap, const char *langtype,
                             uint8_t **out, uint64_t *out_len) {
    return kvspaceXvalueHeadNewSlack(body, body_len, cap, langtype, out,
                                     out_len);
}

int kvspaceXvalueHeadReserve(uint64_t body_len, uint64_t cap,
                             const char *langtype,
                             uint8_t **out, uint64_t *out_len) {
    /* 预留 = 产 cap 字节零 body 的占位（后由调用方填充前 body_len 字节），
       attr 区（含 langtype 与 reallen）一并写入。 */
    if (cap < body_len)
        return fail(out);
    const uint8_t layout = cap > body_len ? KVSPACE_XVALUE_LAYOUT_CAPACITY
                                          : KVSPACE_XVALUE_LAYOUT_LENGTH;
    return build(layout, NULL, cap, body_len, 0, langtype, out, out_len);
}
