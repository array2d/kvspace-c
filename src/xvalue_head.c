#include "xvalue_head.h"

#include <stdlib.h>
#include <string.h>

/* V2 最终态（#566）：head 固定 8 字节 [len:u48][storetype:u8][ptr:u8]，
   body 偏移恒 = 8。类型/形状（含 dims）不在 head——明文 langtype 串存 x‥langtype
   attr key。固定布局：直接 memcpy struct + body。 */

#define UINT48_MAX UINT64_C(0xFFFFFFFFFFFF)

static int build(uint8_t storetype, uint8_t ptr, const uint8_t *body,
                 uint64_t len, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len || (len && !body))
        return -1;
    *out = NULL;
    if (storetype > KVSPACE_XVALUE_ST_EXT || ptr > KVSPACE_XVALUE_PTR_EXT)
        return -1;
    if (len > UINT48_MAX)
        return -1;
    uint64_t n = (uint64_t)KVSPACE_XVALUE_HEAD_LEN + len;
    if (n > SIZE_MAX)
        return -1;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)n);
    if (!p)
        return -1;
    kvspaceXvalueHead h = {.storetype = storetype, .ptr = ptr};
    kvspaceXvalueHeadSetLen(&h, len);
    memcpy(p, &h, sizeof h);
    if (len)
        memcpy(p + KVSPACE_XVALUE_HEAD_LEN, body, len);
    *out = p;
    *out_len = n;
    return 0;
}

int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len) {
    return build(KVSPACE_XVALUE_ST_SHORT, KVSPACE_XVALUE_PTR_INLINE, NULL, 0,
                 out, out_len);
}

int kvspaceXvalueHeadNewScalar(uint8_t storetype, const uint8_t *raw,
                               uint32_t raw_len, uint8_t **out, uint64_t *out_len) {
    if (raw_len && !raw)
        return -1;
    return build(storetype, KVSPACE_XVALUE_PTR_INLINE, raw, raw_len, out, out_len);
}

int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              uint64_t cap, uint8_t **out, uint64_t *out_len) {
    if (cap < data_len || data_len > UINT48_MAX || (data_len && !data))
        return -1;
    (void)cap;
    return build(KVSPACE_XVALUE_ST_SLACK, KVSPACE_XVALUE_PTR_INLINE, data,
                 data_len, out, out_len);
}

int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               uint8_t **out, uint64_t *out_len) {
    if (!dims || ndim == 0 || ndim > 64 || raw_len > UINT48_MAX ||
        (raw_len && !raw))
        return -1;
    (void)dims;
    (void)ndim;
    /* 形状在 attr key langtype；body 字节数 = raw_len（明文层校验 numel×width） */
    return build(KVSPACE_XVALUE_ST_LARGE, KVSPACE_XVALUE_PTR_INLINE, raw,
                 raw_len, out, out_len);
}

int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            uint8_t **out, uint64_t *out_len) {
    if (!path_len || !path)
        return -1;
    return build(KVSPACE_XVALUE_ST_SLACK, KVSPACE_XVALUE_PTR_PTR, path, path_len,
                 out, out_len);
}

int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            uint8_t **out, uint64_t *out_len) {
    if (!locator_len || !locator)
        return -1;
    return build(KVSPACE_XVALUE_ST_EXT, KVSPACE_XVALUE_PTR_EXT, locator,
                 locator_len, out, out_len);
}

int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             uint64_t cap, uint8_t **out, uint64_t *out_len) {
    if (body_len > cap || body_len > UINT48_MAX || (body_len && !body))
        return -1;
    (void)cap;
    return build(KVSPACE_XVALUE_ST_SLACK, KVSPACE_XVALUE_PTR_INLINE, body,
                 body_len, out, out_len);
}

int kvspaceXvalueHeadReserve(uint8_t storetype, uint64_t body_len,
                             uint64_t cap, uint8_t **out, uint64_t *out_len) {
    if (body_len > UINT48_MAX || cap < body_len)
        return -1;
    /* 固定头无容量字段：预留 = 产 body_len 字节零 body 的占位（后由调用方填充）。 */
    (void)cap;
    if (body_len > SIZE_MAX - KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    uint64_t n = (uint64_t)KVSPACE_XVALUE_HEAD_LEN + body_len;
    if (n > SIZE_MAX)
        return -1;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)n);
    if (!p)
        return -1;
    kvspaceXvalueHead h = {.storetype = storetype,
                           .ptr = KVSPACE_XVALUE_PTR_INLINE};
    kvspaceXvalueHeadSetLen(&h, body_len);
    memcpy(p, &h, sizeof h);
    *out = p;
    *out_len = n;
    return 0;
}
