/* Wire: [bodylen:u64][attrslen:u32][storetype:u8][ptr:u8][pad:2][body][attrs].
   Head is fixed 16 bytes; body offset is always 16; the attr region follows the
   body and carries NO length prefix — attrslen is in the head, so a second
   prefix would be a second source of truth. Total = 16 + bodylen + attrslen,
   computed from the head alone.
   Type rides in the same single-key value — no attr key. attr record =
   [tag:u8][len:u16][data]; tag 1 = langtype, tags 2+ reserved (ro/vid...).
   Wire is little-endian. Current platforms are LE, so a straight memcpy of the
   struct equals the wire; a big-endian target must convert at the boundary. */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_XVALUE_HEAD_LEN 16u

/* storetype：存储形态（= 旧 class） */
#define KVSPACE_XVALUE_ST_SHORT 0u   /* 短定长（标量/None/map 头等，body 宽度由类型定） */
#define KVSPACE_XVALUE_ST_SLACK 1u   /* 变长（字节/字符数组/代码槽/指针路径，len=有效字节） */
#define KVSPACE_XVALUE_ST_LARGE 2u   /* 大定长（张量，body=元素连续） */
#define KVSPACE_XVALUE_ST_EXT   3u   /* 扩展定位（@ext，body=定位串） */
/* 定容量变长：**bodylen 是容量**（与 0–3 语义不同），有效长度在 attr tag 2。 */
#define KVSPACE_XVALUE_ST_VARLEN 4u

/* ptr：指针/扩展形态 */
#define KVSPACE_XVALUE_PTR_INLINE 0u
#define KVSPACE_XVALUE_PTR_PTR    1u
#define KVSPACE_XVALUE_PTR_EXT    2u

/* Fixed 16-byte layout (direct memcpy struct + body + attrs).
   wire: bodylen:u64@0, attrslen:u32@8, storetype@12, ptr@13, pad@14-15. */
typedef struct kvspaceXvalueHead {
    uint64_t bodylen;      /* body 字节数 */
    uint32_t attrslen;     /* attr 区字节数（不含任何前缀） */
    uint8_t  storetype;    /* 存储形态 */
    uint8_t  ptr;          /* 指针/扩展形态 */
    uint8_t  padding[2];   /* 必须为零；读取方遇到非零按非法处理 */
} kvspaceXvalueHead;

_Static_assert(sizeof(kvspaceXvalueHead) == KVSPACE_XVALUE_HEAD_LEN,
               "XValue head must be exactly 16 bytes");
_Static_assert(offsetof(kvspaceXvalueHead, bodylen) == 0U, "bodylen@0");
_Static_assert(offsetof(kvspaceXvalueHead, attrslen) == 8U, "attrslen@8");
_Static_assert(offsetof(kvspaceXvalueHead, storetype) == 12U, "storetype@12");
_Static_assert(offsetof(kvspaceXvalueHead, ptr) == 13U, "ptr@13");
_Static_assert(offsetof(kvspaceXvalueHead, padding) == 14U, "padding@14");

/* 字段的唯一定义点：读/写一律走这些 inline 方法，禁止各处直接取字段或手写偏移。
   实现退化成一个字段读取，但 helper 必须保留——否则直接访问会散到各处。 */
static inline uint64_t kvspaceXvalueHeadLen(const kvspaceXvalueHead *h) {
    return h->bodylen;
}
static inline void kvspaceXvalueHeadSetLen(kvspaceXvalueHead *h, uint64_t v) {
    h->bodylen = v;
}
static inline uint32_t kvspaceXvalueHeadAttrsLen(const kvspaceXvalueHead *h) {
    return h->attrslen;
}
static inline void kvspaceXvalueHeadSetAttrsLen(kvspaceXvalueHead *h, uint32_t v) {
    h->attrslen = v;
}
/* padding 是 ABI 的一部分：非零即非法，读取路径必须先过这一关。 */
static inline int kvspaceXvalueHeadPaddingIsZero(const kvspaceXvalueHead *h) {
    return h->padding[0] == 0u && h->padding[1] == 0u;
}
/* 总长 = 16 + bodylen + attrslen；溢出或非零 padding 都返回 0（0 不是合法总长）。 */
static inline uint64_t kvspaceXvalueHeadTotalLen(const kvspaceXvalueHead *h) {
    if (!kvspaceXvalueHeadPaddingIsZero(h))
        return 0;
    uint64_t body = kvspaceXvalueHeadLen(h);
    uint64_t attrs = kvspaceXvalueHeadAttrsLen(h);
    if (body > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN ||
        attrs > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN - body)
        return 0;
    return (uint64_t)KVSPACE_XVALUE_HEAD_LEN + body + attrs;
}
/* attr 区起点：总由 head 推出，不传 body_len。 */
static inline const uint8_t *kvspaceXvalueHeadAttrs(const uint8_t *wire) {
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    return wire + KVSPACE_XVALUE_HEAD_LEN + (uint64_t)h->bodylen;
}

/* attr 区常量：record tag 1 = langtype；tag 2 = reallen（storetype 4 专用）；其余预留。 */
#define KVSPACE_XVALUE_ATTR_TAG_LANGTYPE 1u
/* tag 2 的归属是**排他**的：storetype 4 必须带、其它 storetype 必须不带。
   未知 tag 一般忽略以保持前向兼容；tag 2 例外——它已分配给 reallen，而 reallen
   只在 storetype 4 下有定义。「有定义但在此 storetype 下无意义」说明写读双方对格式
   理解不一致，必须拒绝；「我不认识这个 tag」才该忽略。 */
#define KVSPACE_XVALUE_ATTR_TAG_REALLEN 2u


/* Scan the attr region for tag=1 (langtype) data; returns 0 if missing or
   malformed. Unbounded: caller must ensure the wire holds 16+bodylen+attrslen
   bytes; use kvspaceXvalueHeadLangtypeBounded when bounds are not yet verified.
   The attr region has no length prefix — its size comes from the head. */
static inline uint16_t kvspaceXvalueHeadLangtype(const uint8_t *wire,
                                                 const uint8_t **out) {
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    *out = NULL;
    if (!kvspaceXvalueHeadPaddingIsZero(h))
        return 0;
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + h->bodylen;
    const uint8_t *end = p + h->attrslen;
    while (p + 3 <= end) {
        uint8_t tag = p[0];
        uint16_t rlen = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if (p + 3 + rlen > end)
            return 0;
        if (tag == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE) {
            *out = p + 3;
            return rlen;
        }
        p += 3 + rlen;
    }
    return 0;
}

/* Bounded variant: verifies the head fits, padding is zero, and
   16+bodylen+attrslen <= wire_len before scanning records.
   Returns 0 and sets *out=NULL on missing/malformed/truncated values. */
static inline uint16_t kvspaceXvalueHeadLangtypeBounded(const uint8_t *wire,
                                                        uint64_t wire_len,
                                                        const uint8_t **out) {
    if (out)
        *out = NULL;
    if (!wire || wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return 0;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_len)
        return 0;
    return kvspaceXvalueHeadLangtype(wire, out);
}

/* Constructors (fixed 16-byte header + body + attr region in a single value).
   Empty/NULL langtype omits the langtype record (attrslen = 0).
   Returns 0 on success, -1 and clears *out on failure (when out is non-NULL).
   Fixed header has no capacity/shape fields: cap only checks cap >= len, and
   dims/ndim only check non-empty (shape lives in leading [dims] of langtype).
   NewCode is an alias of NewSlack (SLACK/INLINE) kept for ABI compatibility. */
int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewScalar(uint8_t storetype, const uint8_t *raw,
                               uint32_t raw_len, const char *langtype,
                               uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              uint64_t cap, const char *langtype,
                              uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             uint64_t cap, const char *langtype,
                             uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadReserve(uint8_t storetype, uint64_t body_len,
                             uint64_t cap, const char *langtype,
                             uint8_t **out, uint64_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
