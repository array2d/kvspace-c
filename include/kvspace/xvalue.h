/*
 * xvalue.h — XValue 类型系统与 TLV 编解码（对齐 kvspace-durable 的 kindexp TLV）。
 *
 * TLV: [1B kindexprlen][kindexpr 含 0x00 padding][1B ro][4B vid LE][4B raw_len LE][raw]
 *   kindexpr 串首字节 * =软链接(Ptr, raw=目标路径) / @ =扩展句柄 / 无 =内联，其后 [d0,d1]kind 承载 ndim+dims：
 *   裸 kind=标量(ndim=0)、[n]kind=一维、[d0,d1]kind=多维。kindexprlen 为槽总长（含 padding），
 *   reshape 时新 kindexpr 不超过槽长即可原地改写不搬 body；内容以首个 NUL 终止。
 *   char/* kind 恒为一维序列（[n]，含空串/单字符）
 * None 编码为 NULL/len=0。
 */

#ifndef XVALUE_H
#define XVALUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define KVSPACE_KIND_NONE       ""
#define KVSPACE_KIND_BOOL       "bool"
#define KVSPACE_KIND_INT8       "int8"
#define KVSPACE_KIND_INT16      "int16"
#define KVSPACE_KIND_INT32      "int32"
#define KVSPACE_KIND_INT64      "int64"
#define KVSPACE_KIND_UINT8      "uint8"
#define KVSPACE_KIND_UINT16     "uint16"
#define KVSPACE_KIND_UINT32     "uint32"
#define KVSPACE_KIND_UINT64     "uint64"
#define KVSPACE_KIND_FLOAT32    "float32"
#define KVSPACE_KIND_FLOAT64    "float64"
#define KVSPACE_KIND_CHAR       "char/utf32"
#define KVSPACE_KIND_CHAR_UTF8  "char/utf8"
#define KVSPACE_KIND_CHAR_ASCII "char/ascii"
#define KVSPACE_KIND_OBJ        "objindex"
#define KVSPACE_KIND_MAP        "strkeymapindex"
#define KVSPACE_KIND_INDEX      "index"
#define KVSPACE_KIND_EXT_INDEX  "extindex"
#define KVSPACE_KIND_RWIR       "rwir"
#define KVSPACE_KIND_RWFUNC     "rwfunc"
#define KVSPACE_KIND_DEF_RWIR   "defrwir"
#define KVSPACE_KIND_DEF_RWFUNC "defrwfunc"
#define KVSPACE_KIND_SCOPE      "scope"
#define KVSPACE_KIND_TIME       "time"
#define KVSPACE_KIND_DURATION   "duration"

#define X_MAX_NDIM 8

typedef struct {
    const char    *kindexpr;     /* kindexpr 内容（data 内，含 ref 前缀与 [dims]，非 NUL 终止） */
    int32_t        kindexpr_len; /* kindexpr 内容长度（去 padding，扫到 NUL） */
    int32_t        kindexprlen;  /* wire 槽总长（内容 + NUL + padding） */
    const char    *kind;         /* 派生：base kind（kindexpr 子串），非 NUL 终止 */
    int32_t        kind_len;
    int32_t        ref;          /* 派生：0=内联 1=软链接 2=扩展句柄 */
    int32_t        ro;           /* 1=只读，0=可写 */
    uint32_t       vid;          /* vthread id（默认 0） */
    int32_t        ndim;         /* 派生：0=标量，N=N 维数组 */
    int32_t        dims[X_MAX_NDIM];
    int32_t        array_len;    /* 派生：标量=1，定长=∏dims */
    int32_t        raw_len;
    const uint8_t *raw;
} xvalue_head_t;

/* head 字节数（不含 body） */
int32_t kvspaceXvalueHeadLen(const xvalue_head_t *h);

/* 内联编码（ref=0）。dims/ndim 直接落盘：ndim=0 标量，dims 可为 NULL。 */
int32_t kvspaceXvalueEncode(const char *kind, const uint8_t *raw, int32_t raw_len,
                      const int32_t *dims, int32_t ndim, uint8_t **out);
/* 软链接编码（ref=1），raw 为目标路径。 */
int32_t kvspaceXvalueEncodePtr(const char *kind, const uint8_t *raw, int32_t raw_len,
                          const int32_t *dims, int32_t ndim, uint8_t **out);
/* 带权限编码（ref + ro + vid）。 */
int32_t kvspaceXvalueEncodeMode(const char *kind, const uint8_t *raw, int32_t raw_len,
                          const int32_t *dims, int32_t ndim, int32_t ref, int32_t ro, uint32_t vid,
                          uint8_t **out);
xvalue_head_t kvspaceXvalueDecodeHead(const uint8_t *data, int32_t data_len);

/* raw 读取 helpers（小端） */
static inline int8_t   kvspaceXvalueRawInt8(const uint8_t *r)   { return (int8_t)r[0]; }
static inline int16_t  kvspaceXvalueRawInt16(const uint8_t *r)  { return (int16_t)(r[0]|(r[1]<<8)); }
static inline int32_t  kvspaceXvalueRawInt32(const uint8_t *r)  { return (int32_t)(r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24)); }
static inline int64_t  kvspaceXvalueRawInt64(const uint8_t *r)  { return (int64_t)r[0]|((int64_t)r[1]<<8)|((int64_t)r[2]<<16)|((int64_t)r[3]<<24)|((int64_t)r[4]<<32)|((int64_t)r[5]<<40)|((int64_t)r[6]<<48)|((int64_t)r[7]<<56); }
static inline uint8_t  kvspaceXvalueRawUint8(const uint8_t *r)  { return r[0]; }
static inline uint16_t kvspaceXvalueRawUint16(const uint8_t *r) { return (uint16_t)(r[0]|(r[1]<<8)); }
static inline uint32_t kvspaceXvalueRawUint32(const uint8_t *r) { return (uint32_t)(r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24)); }
static inline uint64_t kvspaceXvalueRawUint64(const uint8_t *r) { return (uint64_t)r[0]|((uint64_t)r[1]<<8)|((uint64_t)r[2]<<16)|((uint64_t)r[3]<<24)|((uint64_t)r[4]<<32)|((uint64_t)r[5]<<40)|((uint64_t)r[6]<<48)|((uint64_t)r[7]<<56); }
static inline float  kvspaceXvalueRawFloat32(const uint8_t *r) { union{uint32_t u;float f;}v;v.u=kvspaceXvalueRawUint32(r);return v.f;}
static inline double kvspaceXvalueRawFloat64(const uint8_t *r) { union{uint64_t u;double f;}v;v.u=kvspaceXvalueRawUint64(r);return v.f;}

static inline int8_t   kvspaceXvalueAtInt8(const xvalue_head_t *h, int32_t idx)   { return kvspaceXvalueRawInt8(h->raw+idx); }
static inline int16_t  kvspaceXvalueAtInt16(const xvalue_head_t *h, int32_t idx)  { return kvspaceXvalueRawInt16(h->raw+idx*2); }
static inline int32_t  kvspaceXvalueAtInt32(const xvalue_head_t *h, int32_t idx)  { return kvspaceXvalueRawInt32(h->raw+idx*4); }
static inline int64_t  kvspaceXvalueAtInt64(const xvalue_head_t *h, int32_t idx)  { return kvspaceXvalueRawInt64(h->raw+idx*8); }
static inline uint8_t  kvspaceXvalueAtUint8(const xvalue_head_t *h, int32_t idx)  { return kvspaceXvalueRawUint8(h->raw+idx); }
static inline uint16_t kvspaceXvalueAtUint16(const xvalue_head_t *h, int32_t idx) { return kvspaceXvalueRawUint16(h->raw+idx*2); }
static inline uint32_t kvspaceXvalueAtUint32(const xvalue_head_t *h, int32_t idx) { return kvspaceXvalueRawUint32(h->raw+idx*4); }
static inline uint64_t kvspaceXvalueAtUint64(const xvalue_head_t *h, int32_t idx) { return kvspaceXvalueRawUint64(h->raw+idx*8); }
static inline float    kvspaceXvalueAtFloat32(const xvalue_head_t *h, int32_t idx) { return kvspaceXvalueRawFloat32(h->raw+idx*4); }
static inline double   kvspaceXvalueAtFloat64(const xvalue_head_t *h, int32_t idx) { return kvspaceXvalueRawFloat64(h->raw+idx*8); }
static inline bool     kvspaceXvalueAtBool(const xvalue_head_t *h, int32_t idx)    { return h->raw[idx] != 0; }

int32_t kvspaceXvalueNewNone(uint8_t **out);

int32_t kvspaceXvalueNewBool(const bool *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewInt8(const int8_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewInt16(const int16_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewInt32(const int32_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewInt64(const int64_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewUint8(const uint8_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewUint16(const uint16_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewUint32(const uint32_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewUint64(const uint64_t *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewFloat32(const float *vals, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewFloat64(const double *vals, int32_t count, uint8_t **out);

/* char/utf32：UTF-8 字符串 → UTF-32 LE 码点（4B×N），array_len=码点数 */
int32_t kvspaceXvalueNewChar(const char *s, uint8_t **out);
/* char/utf8：UTF-8 字节（1B×N） */
int32_t kvspaceXvalueNewCharUtf8(const char *s, uint8_t **out);
/* char/ascii：ASCII 字节（1B×N） */
int32_t kvspaceXvalueNewCharAscii(const char *s, uint8_t **out);
/* 第 idx 个码点（char/utf32） */
int32_t kvspaceXvalueAtChar(const xvalue_head_t *h, int32_t idx);

int32_t kvspaceXvalueNewIndex(const char **children, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewPtr(const char *kind, const char *target, int32_t array_len, uint8_t **out);
int32_t kvspaceXvalueNewExtindex(const char *extpath, const char **children, int32_t count, uint8_t **out);

#define kvspaceXvalueNewBool1(v, out)     kvspaceXvalueNewBool(&(bool){v}, 1, out)
#define kvspaceXvalueNewInt81(v, out)     kvspaceXvalueNewInt8(&(int8_t){v}, 1, out)
#define kvspaceXvalueNewInt161(v, out)    kvspaceXvalueNewInt16(&(int16_t){v}, 1, out)
#define kvspaceXvalueNewInt321(v, out)    kvspaceXvalueNewInt32(&(int32_t){v}, 1, out)
#define kvspaceXvalueNewInt641(v, out)    kvspaceXvalueNewInt64(&(int64_t){v}, 1, out)
#define kvspaceXvalueNewUint81(v, out)    kvspaceXvalueNewUint8(&(uint8_t){v}, 1, out)
#define kvspaceXvalueNewUint161(v, out)   kvspaceXvalueNewUint16(&(uint16_t){v}, 1, out)
#define kvspaceXvalueNewUint321(v, out)   kvspaceXvalueNewUint32(&(uint32_t){v}, 1, out)
#define kvspaceXvalueNewUint641(v, out)   kvspaceXvalueNewUint64(&(uint64_t){v}, 1, out)
#define kvspaceXvalueNewFloat321(v, out)  kvspaceXvalueNewFloat32(&(float){v}, 1, out)
#define kvspaceXvalueNewFloat641(v, out)  kvspaceXvalueNewFloat64(&(double){v}, 1, out)

#endif
