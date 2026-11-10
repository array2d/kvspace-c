/* kvspace-c **内部**辅助：从 wire 读「这个值是不是引用」与「有效长度」。
 *
 * 为什么不是公共头里的函数：用户要求**不加任何 ABI 符号**。格式知识在三个实现里
 * 各有一份（Rust / 本仓 / runtimec），一致性由**同一组测试矩阵**保证，不靠共享符号。
 *
 * 数值一律从常量取（`KVSPACE_XVALUE_ATTR_TAG_*`），**不在这里写死 1 或 2**——
 * 格式知识可以重复，字面量不行。
 *
 * 本文件**不安装**，只供本仓 .c 与测试使用。 */
#ifndef KVSPACE_C_XVALUE_INTERNAL_H
#define KVSPACE_C_XVALUE_INTERNAL_H

#include <kvspace/kvlang_xvalue.h>

/* 引用性 —— 从 langtype 的**首字节**判定。
 *
 * 旧布局把「是不是引用」放在 head 的 `ptr` 字节里；新布局没有这个字段，
 * 它由 langtype 前缀 `*`（Ptr）与 `@`（Ext）表达（见 `kvlang_xvalue.h` 里
 * `NewPtr`/`NewExt` 的注释：它们是唯一知道「这个值是引用」的构造点，
 * 构造器补前缀）。
 *
 * **这是全仓唯一一处前缀判定**：6 个调用点都调它，不各写一份。
 *
 * 返回：`KVSPACE_XVALUE_REFERENCE_*` 之一。langtype 读不出来（wire 畸形）⇒
 * `INVALID`，不猜。
 */
#define KVSPACE_XVALUE_REFERENCE_INVALID 0
#define KVSPACE_XVALUE_REFERENCE_INLINE 1
#define KVSPACE_XVALUE_REFERENCE_PTR 2
#define KVSPACE_XVALUE_REFERENCE_EXT 3

/* 已删除的 tag 2（旧 reallen）：任何位置出现都算畸形。
 * 上游实现里这个常量是私有的（`src/xvalue_head.c`），本仓要在读取路径上
 * 拒绝它，故在此重复一次。**重复的不是格式知识，是一条拒绝规则**——
 * 值必须与上游一致（2）。 */
#define KVSPACE_C_ATTR_TAG_RETIRED_REALLEN 2u

/* 扫完整条 attr 区并取出 tag 1（langtype）：返回 0 成功，-1 畸形。
 *
 * 这是本仓的**唯一读取闸口**，替代已被删除的 `kvspaceXvalueHeadScan`。
 * 为什么不直接用上游的 `kvspaceXvalueGetAttr`：它是「找某个 tag」，
 * **找到就返回**，不会继续扫后面的记录——于是「tag 1 在前、已废的 tag 2 在后」
 * 这种畸形会被它放过（实测：`GetAttr(tag1)` 返回 0）。而读取路径要的是
 * 「整区自洽」这个更强的判据，所以这里自己扫完整区。
 *
 * 输出参数都可为 NULL。语义与被删的 Scan 一致：重复 tag 1 取第一条，
 * 尾部不足一条记录的残余字节忽略（与 Rust 侧同语义）。 */
static inline int kvspace_c_scan_attrs(const uint8_t *wire, uint64_t wire_len,
                                       const uint8_t **out_langtype,
                                       uint16_t *out_langtype_len) {
    if (out_langtype)
        *out_langtype = NULL;
    if (out_langtype_len)
        *out_langtype_len = 0;
    if (!wire || wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_len)
        return -1;
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + kvspaceXvalueHeadLen(h);
    const uint8_t *end = p + kvspaceXvalueHeadAttrsLen(h);
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    while (p + 3 <= end) {
        const uint8_t tag = p[0];
        const uint16_t rlen = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if (p + 3 + rlen > end)
            return -1;
        if (tag == KVSPACE_C_ATTR_TAG_RETIRED_REALLEN)
            return -1; /* 已删除的格式：出现即拒，不猜 */
        if (tag == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && !langtype) {
            langtype = p + 3;
            langtype_len = rlen;
        }
        p += 3 + rlen;
    }
    if (out_langtype)
        *out_langtype = langtype;
    if (out_langtype_len)
        *out_langtype_len = langtype_len;
    return 0;
}

static inline int kvspace_c_reference_of(const uint8_t *wire, uint64_t wire_len) {
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;

    if (kvspace_c_scan_attrs(wire, wire_len, &langtype, &langtype_len) != 0)
        return KVSPACE_XVALUE_REFERENCE_INVALID;
    if (langtype == NULL || langtype_len == 0)
        return KVSPACE_XVALUE_REFERENCE_INLINE; /* 空 langtype = 不是引用 */
    if (langtype[0] == '*')
        return KVSPACE_XVALUE_REFERENCE_PTR;
    if (langtype[0] == '@')
        return KVSPACE_XVALUE_REFERENCE_EXT;
    return KVSPACE_XVALUE_REFERENCE_INLINE;
}

/* 有效长度 = `bodylen`（head 里已没有容量之说，wire 里也没有 tag 2）。
 *
 * 上游头的 `kvspaceXvalueHeadEffectiveLen` 已经做这件事（含 padding 与总长的
 * 全部校验），本仓直接用它——**不在这里维护第二份**。保留本包装只为
 * 让调用点不必各自写「传 NULL 取错误」的样板。
 *
 * 返回 0 成功；非 0 失败（畸形 wire）。**错误不走长度值**：0 是合法长度。 */
static inline int kvspace_c_effective_len(const uint8_t *wire, uint64_t total,
                                          uint64_t *out) {
    return kvspaceXvalueHeadEffectiveLen(wire, total, out);
}

/* head 与 attrs 区是否逐字节相同 —— 原地写的前置条件。
 *
 * 判据不是「总长相同」：`int64 → float64` 两者都是 8 字节，总长一样但**类型变了**。
 * 类型在 attr 的 langtype 记录里，所以 attrs 区必须一并比。这也是既有契约
 * 「原地写只改 body」的直接表达：head 与 attrs 不变，能改的就只剩 body。
 *
 * 两侧都必须是自洽 wire（`kvspaceXvalueHeadTotalLen` 非 0）。 */
static inline int kvspace_c_same_shape(const uint8_t *left, uint64_t left_len,
                                       const uint8_t *right, uint64_t right_len) {
    const kvspaceXvalueHead *left_head;
    const kvspaceXvalueHead *right_head;
    uint64_t left_total;
    uint64_t right_total;

    if (!left || !right || left_len < KVSPACE_XVALUE_HEAD_LEN ||
        right_len < KVSPACE_XVALUE_HEAD_LEN)
        return 0;
    left_head = (const kvspaceXvalueHead *)(const void *)left;
    right_head = (const kvspaceXvalueHead *)(const void *)right;
    left_total = kvspaceXvalueHeadTotalLen(left_head);
    right_total = kvspaceXvalueHeadTotalLen(right_head);
    if (left_total == 0 || right_total == 0 || left_total > left_len ||
        right_total > right_len)
        return 0;
    if (kvspaceXvalueHeadLen(left_head) != kvspaceXvalueHeadLen(right_head))
        return 0;
    if (kvspaceXvalueHeadAttrsLen(left_head) != kvspaceXvalueHeadAttrsLen(right_head))
        return 0;
    return memcmp(left, right, KVSPACE_XVALUE_HEAD_LEN) == 0 &&
           memcmp(left + KVSPACE_XVALUE_HEAD_LEN + kvspaceXvalueHeadLen(left_head),
                  right + KVSPACE_XVALUE_HEAD_LEN + kvspaceXvalueHeadLen(right_head),
                  kvspaceXvalueHeadAttrsLen(left_head)) == 0;
}

#endif
