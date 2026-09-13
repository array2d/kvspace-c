/*
 * xvalue.h — XValue 类型系统与三轴 head 编解码（对齐 kvspace/frontend.c 黄金基准 + kvspace-durable）。
 *
 * head = [headlen u16 LE][ref u8][storetype u8][ro u8][vid u32 LE][body_len u32 LE]
 *        [storetype 物理字段][langtype kindexpr 串（占至 headlen）]
 * body = [body_len B raw]
 *   ref       0=inline（body=值本体）/1=ptr（body=目标 key）/2=@ext（body=扩展定位符）
 *   storetype NONE/ATOM/ARRAYND/index/extindex；codec 唯一分派。
 *             物理字段：ARRAYND / index / extindex 为 ndim u8 + dims[ndim] u32 LE
 *             （index/extindex 的 dims=[len,cap,M]）；NONE / ATOM 无物理字段。
 *   langtype  完整 kindexpr 串，恒为 head 最后一段（长度 = headlen − 当前偏移，无独立长度字段）。
 *             ARRAYND 含 [dims]；ATOM/index/extindex 为裸种类名/路径；None 为空串。
 *   Ptr: langtype=目标完整 kindexpr、body=目标 key、物理字段恒空。char/* 一维序列 → ARRAYND。
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
#define KVSPACE_KIND_MAP        "stringkeymap"
#define KVSPACE_KIND_INDEX      "index"
#define KVSPACE_KIND_EXT_INDEX  "extindex"
#define KVSPACE_KIND_RWIR       "rwir"
#define KVSPACE_KIND_RWFUNC     "rwfunc"
#define KVSPACE_KIND_DEF_RWIR   "def rwir"
#define KVSPACE_KIND_SCOPE      "scope"
#define KVSPACE_KIND_TIME       "time"
#define KVSPACE_KIND_DURATION   "duration"

#define X_MAX_NDIM 8

/* ref：存储位置维（head 第 2 字节）。 */
#define KVSPACE_REF_INLINE 0  /* body = 值本体 raw */
#define KVSPACE_REF_PTR    1  /* body = 目标 key 路径（软链接） */
#define KVSPACE_REF_EXT    2  /* body = 扩展世界定位符 */

/* storetype：物理布局维（head 第 3 字节，codec 唯一分派）。 */
#define KVSPACE_STORETYPE_NONE     0  /* 无物理字段，body 空 */
#define KVSPACE_STORETYPE_ATOM     1  /* 无物理字段，body 定宽 raw */
#define KVSPACE_STORETYPE_ARRAYND  2  /* ndim u8 + dims[ndim] u32 LE，body 稠密数组 */
#define KVSPACE_STORETYPE_INDEX    3  /* 成员名矩阵 dims=[len,cap,M] */
#define KVSPACE_STORETYPE_EXTINDEX 4  /* 同 index，cap 可增长 */

typedef struct {
    uint16_t       headlen;      /* head 总字节数；body 起于偏移 headlen */
    int32_t        ref;          /* 存储位置：见 KVSPACE_REF_* */
    uint8_t        storetype;    /* 物理布局：见 KVSPACE_STORETYPE_* */
    const char    *langtype;     /* langtype kindexpr 串（data 内，含 [dims]、无前缀，非 NUL 终止） */
    int32_t        langtype_len; /* langtype 内容长度 */
    const char    *kind;         /* 派生：base 种类名（越过 [dims]，langtype 子串），非 NUL 终止 */
    int32_t        kind_len;
    int32_t        ro;           /* 1=只读，0=可写 */
    uint32_t       vid;          /* vthread id（默认 0） */
    int32_t        ndim;         /* ARRAYND：维数；index/extindex：3；NONE/ATOM：0 */
    int32_t        dims[X_MAX_NDIM]; /* ARRAYND：各维长；index/extindex：[len,cap,M] */
    int32_t        array_len;    /* 派生：标量=1，定长=∏dims */
    int32_t        raw_len;      /* body 字节数 */
    const uint8_t *raw;          /* body 指针（data + headlen） */
} xvalue_head_t;

/* head 字节数（不含 body） */
int32_t kvspaceXvalueHeadLen(const xvalue_head_t *h);

/* 由 (storetype, langtype, ndim) 直接算 head 字节数。零拷贝写路径用：ro/vid 恒 0。 */
int32_t kvspaceXvalueHeadLenForLangtype(uint8_t storetype, const char *langtype, int32_t ndim);
/* 把 head（ref + storetype + ro + vid + langtype + 物理字段 + body_len）就地写入 dst 前 headlen
 * 字节；body 随后由调用方填。dims 仅在 store_has_dims(storetype) 时落物理字段。 */
void kvspaceXvalueWriteHead(uint8_t *dst, uint8_t ref, uint8_t storetype, uint8_t ro, uint32_t vid,
                            const char *langtype, const int32_t *dims, int32_t ndim,
                            int32_t body_len);

/* 内联编码（ref=0）。dims/ndim 直接落盘：ndim=0 标量，dims 可为 NULL。 */
int32_t kvspaceXvalueEncode(const char *kind, const uint8_t *raw, int32_t raw_len,
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


int32_t kvspaceXvalueNewIndex(const char **children, int32_t count, uint8_t **out);
/* memindex 定宽矩阵（dims=[len,cap,M]，M 向上 8 对齐）：cap_hint/m_hint 为容量与行宽下限（只增），
 * 供增删时保留预留容量与既有行宽 → body 长度恒 cap×M、就地覆写不重分配。 */
int32_t kvspaceXvalueNewIndexGrow(const char **children, int32_t count, int32_t cap_hint,
                                  int32_t m_hint, uint8_t **out);
int32_t kvspaceXvalueNewPtr(const char *target_kindexpr, const char *target, uint8_t **out);
int32_t kvspaceXvalueNewExtindex(const char *extpath, const char **children, int32_t count, uint8_t **out);
int32_t kvspaceXvalueNewExtindexGrow(const char *extpath, const char **children, int32_t count,
                                     int32_t cap_hint, int32_t m_hint, uint8_t **out);

/* 坐标段比较（对齐 durable coord::cmp_coord）：坐标段（[i]/[i,j]）数值 row-major 在前、
 * 非坐标段字典序在后。供 memindex 矩阵规范排序与容器 dims 派生共用。 */
int kvspaceCoordIsCoord(const char *name);
int kvspaceParseCoord(const char *name, int64_t *coords, int maxn);
int kvspaceCoordCmp(const char *a, const char *b);

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
