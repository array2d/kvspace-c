/* Wire: [len:u48][storetype:u8][ptr:u8][body][attr]. Head is fixed 8 bytes,
   body offset is always 8; attr region starts at 8+len and is length-prefixed
   (total = 8 + len + attr_len). Type rides in the same single-key value — no
   attr key. attr = [attr_len:u16][records]; record = [tag:u8][len:u16][data],
   tag 1 = langtype, tags 2+ reserved (ro/vid...). */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_XVALUE_HEAD_LEN 8u

/* storetype：存储形态（= 旧 class） */
#define KVSPACE_XVALUE_ST_SHORT 0u   /* 短定长（标量/None/map 头等，body 宽度由类型定） */
#define KVSPACE_XVALUE_ST_SLACK 1u   /* 变长（字节/字符数组/代码槽/指针路径，len=有效字节） */
#define KVSPACE_XVALUE_ST_LARGE 2u   /* 大定长（张量，body=元素连续） */
#define KVSPACE_XVALUE_ST_EXT   3u   /* 扩展定位（@ext，body=定位串） */

/* ptr：指针/扩展形态 */
#define KVSPACE_XVALUE_PTR_INLINE 0u
#define KVSPACE_XVALUE_PTR_PTR    1u
#define KVSPACE_XVALUE_PTR_EXT    2u

/* 固定 8 字节布局（直接 memcpy struct + body）。
   wire：len:u48@0-5（小端）、storetype@6、ptr@7、body@8、langtype@8+len。 */
typedef struct kvspaceXvalueHead {
    uint8_t        len[6];        /* u48 小端（offset 0-5） */
    uint8_t        storetype;
    uint8_t        ptr;
} kvspaceXvalueHead;

/* u48 len 的唯一定义点：读/写一律走这两个 inline 方法，禁止各处手写拼接。 */
static inline uint64_t kvspaceXvalueHeadLen(const kvspaceXvalueHead *h) {
    uint64_t v = 0;
    for (int i = 5; i >= 0; i--)
        v = (v << 8) | h->len[i];
    return v;
}
static inline void kvspaceXvalueHeadSetLen(kvspaceXvalueHead *h, uint64_t v) {
    for (int i = 0; i < 6; i++) {
        h->len[i] = (uint8_t)(v & 0xff);
        v >>= 8;
    }
}

/* attr 区常量：record tag 1 = langtype；2+ 预留扩展（ro/vid 等，未实现）。 */
#define KVSPACE_XVALUE_ATTR_TAG_LANGTYPE 1u

/* 读 attr 总长（含自身 2 字节；空 attr = 2）。 */
static inline uint16_t kvspaceXvalueHeadAttrLen(const uint8_t *wire,
                                                uint64_t body_len) {
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + body_len;
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 扫 attr records 取 tag=1（langtype）记录 data；未找到/坏 attr → 返回 0。 */
static inline uint16_t kvspaceXvalueHeadLangtype(const uint8_t *wire,
                                                 uint64_t body_len,
                                                 const uint8_t **out) {
    *out = NULL;
    uint16_t alen = kvspaceXvalueHeadAttrLen(wire, body_len);
    if (alen < 2)
        return 0;
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + body_len + 2;
    const uint8_t *end = wire + KVSPACE_XVALUE_HEAD_LEN + body_len + alen;
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

/* 构造（固定头 + body + attr 区，单 key 整体）。langtype 空/NULL = 无 langtype
   记录（空 attr = [attr_len:2]）。0 on success. -1 and *out cleared on failure. */
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
