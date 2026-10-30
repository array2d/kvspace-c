/* Wire: [len:u48][storetype:u8][ptr:u8][body]. Head is fixed 8 bytes,
   body offset is always 8. Type/shape (including dims) is NOT in the head — it
   lives in the x‥langtype attr key key as a plaintext langtype string. */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

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
   wire：len:u48@0-5（小端）、storetype@6、ptr@7、body@8。 */
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

/* 构造（固定头 + body，不含类型；langtype 由调用方写 x‥langtype attr key）。
   0 on success. -1 and *out cleared on failure. */
int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewScalar(uint8_t storetype, const uint8_t *raw,
                               uint32_t raw_len, uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              uint64_t cap, uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             uint64_t cap, uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadReserve(uint8_t storetype, uint64_t body_len,
                             uint64_t cap, uint8_t **out, uint64_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
