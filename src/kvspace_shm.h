/*
 * kvspace_shm.h — kvspace-c 的 SHM 引擎内部契约（不是公共 ABI）。
 *
 * file-backed mmap, ART 树索引 + slotsboxmalloc 变长存储。公共 ABI 只有
 * <kvspace/kvspace.h> 的 12 个符号；本头的 kvspaceShm* 与 kvspace_t 只给
 * src/kvspace.c 与内部测试使用。
 */

/* Guard must not be KVSPACE_H: that is the canonical ABI header <kvspace/kvspace.h>
 * guard, and colliding would silently skip whichever header is included second. */
#ifndef KVSPACE_SHM_H
#define KVSPACE_SHM_H

#include "coord.h"
#include <kvspace/kvspace.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kvspace kvspace_t;

/* SHM store format version:
 *   4 = ART atomic val word + garbage ring + single-writer demotion + 8B node align;
 *   5 = ART 112-byte block;
 *   6 = XValue fixed 8-byte header + inline attr region (#566/#571, incompatible
 *       with format 5) plus header list_gen for cross-process List cache invalidation.
 *   7 = 16 字节 head（`[bodylen:u64][attrslen:u16][reserved:u16][layout:u8][padding:3]`）。
 *   8 = **16 字节 head，10-15 全为 padding**（`[bodylen:u64][attrslen:u16][padding:6]`，
 *       与 `kvspace/include/kvspace/kvspace_xvalue.h` 一致）：删除 `reserved` 与 `layout`，
 *       `bodylen` 恒为有效字节数；容量是存储后端的物理分配属性，不进入 wire。
 *       7 写的 store 在 byte12 上是 layout=1（非零），按 8 读会落进 padding 被判非法——
 *       所以必须升版本号硬拒，不能原地兼容。
 * Incompatible older versions fail hard on open with a rebuild hint (no migration). */
#define KVSPACE_FORMAT_VERSION 8

/* ================================================================
 * 生命周期
 * ================================================================ */

// Files: <path>, <path>.sbo.head, <path>.sbo.data; all grow on demand.
// data_size = 8*64^k, create only, initial (not maximum) data size.
kvspace_t *kvspaceShmOpen(const char *path, size_t data_size);
void kvspaceShmClose(kvspace_t *kv);

/* ================================================================
 * 单点读写
 * ================================================================ */

// Get: resolve=1 穿透 link，resolve=0 返回 link 本身。
uint8_t *kvspaceShmGet(kvspace_t *kv, const char *key, int resolve,
                       int32_t *out_len);
int kvspaceShmSetPart(kvspace_t *kv, const char *key, uint32_t offset,
                      const uint8_t *buf, uint32_t buf_len);

// Set: 写入 value（XValue 字节）。总是穿透 link 写入 target。
int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                  int32_t val_len);
int kvspaceShmSetValue(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len);
// ListLen: direct child count under prefix. 每次调用直接扫描，不缓存。
int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool expand_ext,
                      int resolve, int32_t *out_count);

/* ================================================================
 * 存储层 ABI 原语（<kvspace/kvspace.h> 的 12 符号由 src/kvspace.c 直接实现）
 *
 * 原语返回公共状态码 KVSPACE_*，没有内部/公共两套枚举，也没有映射层。
 * ================================================================ */

/* anchor 0 = root（与上游 src/anchor.h 同值）。 */
#define KVSPACE_ANCHOR_ROOT UINT64_C(0)

/* 进程内值区基址：地址 = 基址 + value_offset；扩容前后不变。 */
uint8_t *kvspaceShmStorageDataBase(kvspace_t *kv);
int kvspaceShmStorageResolve(kvspace_t *kv, const char *key,
                             uint64_t *out_offset);
/* 公共 anchor 是 key 区的 int64 字节偏移；进入引擎后转换成 ART 块号。
 * 0 = root，由调用方直接走 kvspaceShmStorageResolve，不进本函数。 */
typedef struct kvspaceAnchor {
    int64_t key_offset; /* 公共 ABI 传来的原始偏移，保留用于诊断 */
    int32_t block_id;   /* ART 块号（blockid_bydataoffset 的结果） */
} kvspaceAnchor_t;

/* 带 anchor：只校验节点仍存活；查找仍按绝对 key。节点失效 → ANCHOR_INVALID。 */
int kvspaceShmStorageResolveFrom(kvspace_t *kv, const char *key,
                                 kvspaceAnchor_t anchor, uint64_t *out_offset);
/* 唯一写原语：不存在则创建（清零），存在则容量内原地 / 超容量搬移。 */
int kvspaceShmStorageRealloc(kvspace_t *kv, const char *key,
                             uint64_t body_len, uint64_t attr_len,
                             uint64_t *out_offset);
int kvspaceShmStorageFree(kvspace_t *kv, const char *key);
int kvspaceShmStorageMakeindex(kvspace_t *kv, const char *key);
int kvspaceShmStorageListlen(kvspace_t *kv, const char *key, int64_t *out_count,
                             uint32_t *out_max_name_length);
int kvspaceShmStorageList(kvspace_t *kv, const char *key, char *out_names,
                          uint32_t out_capacity, uint32_t *out_length,
                          uint32_t *out_count, uint64_t *out_generation);

/* ================================================================
 * 目录操作
 * ================================================================ */

// List: resolve=1 穿透 link 列出 target 子节点。
int kvspaceShmList(kvspace_t *kv, const char *prefix, bool expand_ext,
                   int resolve, char ***out_names, int32_t *out_count);

int kvspaceShmDel(kvspace_t *kv, const char *key);
// Deltree: delete base subtree (normalizing one trailing '/' or U+00B7 on prefix
// to base; "/" = root): base plus base/..., base<U+00B7>..., and base<U+2025>...
// (including hidden U+2025 keys). Deletes only the pointer if prefix/base is a link.
int kvspaceShmDeltree(kvspace_t *kv, const char *prefix);
int kvspaceShmCp(kvspace_t *kv, const char *src,
                 const char *dst); // 单 key 拷贝
int kvspaceShmCptree(kvspace_t *kv, const char *src,
                     const char *dst); // 递归子树拷贝
int kvspaceShmCplist(kvspace_t *kv, const char *src,
                     const char *dst); // 浅拷贝：base 值 + 一层 · 成员，不递归
int kvspaceShmMkindex(
    kvspace_t *kv, const char *path,
    uint32_t capacity); // 递归创建目录，capacity 预留叶 index 容量（减少扩容）

/* ================================================================
 * ExtIndex
 * ================================================================ */

int kvspaceShmExtindex(kvspace_t *kv, const char *path, const char *extpath,
                       char *err, uint32_t err_cap);
int kvspaceShmDelextindex(kvspace_t *kv, const char *path); // 移除 extindex

#ifdef P2_TEST_HOOKS
/* 对齐/格式验收（仅测试构建）：返回 -1=通过，>=0=首个坏块 id，-2=块头宽度
 * 不是 8；SetFormatVersion 用于构造"旧 store 必须拒绝"的用例。 */
int64_t kvspaceShmArtAlignCheck(kvspace_t *kv);
void kvspaceShmTestSetFormatVersion(kvspace_t *kv, uint32_t v);
/* benchmark-only：给已存在的父节点 key，反算它在 ART key 区的公共 int64 偏移，
 * 即 `kvspaceResolve` 的 `parent_key_anchor` 可接受的值。节点不存在 / 值槽为空 /
 * 偏移反算不到合法块 → 失败，不造值。 */
int kvspaceShmTestAnchorOf(kvspace_t *kv, const char *parent_key,
                           int64_t *out_anchor);
#endif

#endif
