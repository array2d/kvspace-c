#ifndef KVSPACE_CONST_H
#define KVSPACE_CONST_H

/* KVSpace 常量唯一定义处：分隔符 + kind 串。
 *
 * - `#define KVSPACE_* "…"` 是值唯一来源，C/C++ 编译期引用（支持字面量拼接）。
 * - KVSPACE_KV(X) 列出全部名字，供 kvspaceConst() 运行期查询生成查找表；
 *   Rust 侧用 build.rs 解析 `#define KVSPACE_* "…"` 行生成同名常量，不引入 bindgen。
 */

#define KVSPACE_PATH_SEP           "/"
#define KVSPACE_DIR_INDEX_SUF       "/"
#define KVSPACE_MEMBER_SEP          "·"
#define KVSPACE_RUNTIME_MEMBER_SEP  "\xE2\x80\xA5"

#define KVSPACE_KIND_NONE       "None"
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
#define KVSPACE_KIND_RWIR       "rwir"
#define KVSPACE_KIND_RWFUNC     "rwfunc"
#define KVSPACE_KIND_DEF_RWIR   "def rwir"
#define KVSPACE_KIND_SCOPE      "scope"
#define KVSPACE_KIND_TIME       "time"
#define KVSPACE_KIND_DURATION   "duration"

#define KVSPACE_ERR_DIR_MUST_END_WITH_SLASH "kvspace: index must end with /"
#define KVSPACE_ERR_INVALID_PATH            "kvspace: path must be absolute and canonical"

#define KVSPACE_KV(X) \
    X(KVSPACE_PATH_SEP) \
    X(KVSPACE_DIR_INDEX_SUF) \
    X(KVSPACE_MEMBER_SEP) \
    X(KVSPACE_RUNTIME_MEMBER_SEP) \
    X(KVSPACE_KIND_NONE) \
    X(KVSPACE_KIND_BOOL) \
    X(KVSPACE_KIND_INT8) \
    X(KVSPACE_KIND_INT16) \
    X(KVSPACE_KIND_INT32) \
    X(KVSPACE_KIND_INT64) \
    X(KVSPACE_KIND_UINT8) \
    X(KVSPACE_KIND_UINT16) \
    X(KVSPACE_KIND_UINT32) \
    X(KVSPACE_KIND_UINT64) \
    X(KVSPACE_KIND_FLOAT32) \
    X(KVSPACE_KIND_FLOAT64) \
    X(KVSPACE_KIND_CHAR) \
    X(KVSPACE_KIND_CHAR_UTF8) \
    X(KVSPACE_KIND_CHAR_ASCII) \
    X(KVSPACE_KIND_MAP) \
    X(KVSPACE_KIND_RWIR) \
    X(KVSPACE_KIND_RWFUNC) \
    X(KVSPACE_KIND_DEF_RWIR) \
    X(KVSPACE_KIND_SCOPE) \
    X(KVSPACE_KIND_TIME) \
    X(KVSPACE_KIND_DURATION) \
    X(KVSPACE_ERR_DIR_MUST_END_WITH_SLASH) \
    X(KVSPACE_ERR_INVALID_PATH)

/* 运行期查询：kvspaceConst("KVSPACE_KIND_MAP") -> "stringkeymap"。
 * 供 Go/Python/Rust 扩展经 C ABI 取常量，避免硬编码。 */
const char *kvspaceConst(const char *name);

#endif
