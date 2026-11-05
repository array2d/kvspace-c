/* kvspace-c **内部**辅助：storetype 4（定容量变长）的 reallen 解析。
 *
 * 为什么不是公共头里的函数：用户要求**不加任何 ABI 符号**。格式知识在三个实现里
 * 各有一份（Rust / 本仓 / runtimec），一致性由**同一组测试矩阵**保证，不靠共享符号。
 *
 * 数值一律从常量取（`KVSPACE_XVALUE_ATTR_TAG_REALLEN` / `KVSPACE_XVALUE_ST_VARLEN`），
 * **不在这里写死 2 或 4**——格式知识可以重复，字面量不行。
 *
 * 本文件**不安装**，只供本仓 .c 与测试使用。 */
#ifndef KVSPACE_C_XVALUE_INTERNAL_H
#define KVSPACE_C_XVALUE_INTERNAL_H

#include "xvalue_head.h"

/* 有效长度：storetype 4 → attr tag 2 的 reallen；其它 storetype → bodylen。
   返回 0 = 成功；非 0 = 失败（越界 / padding 非零 / 缺 tag 2 / 长度非 8 / 重复 /
   reallen > bodylen）。**错误不走长度值**：0 是合法 reallen。 */
static inline int kvspace_c_effective_len(const uint8_t *wire, uint64_t total,
                                          uint64_t *out) {
    if (out)
        *out = 0;
    if (!wire || !out || total < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    kvspaceXvalueHead h;
    memcpy(&h, wire, sizeof h);
    if (!kvspaceXvalueHeadPaddingIsZero(&h))
        return -1;
    const uint64_t body = h.bodylen;
    const uint64_t attrs = h.attrslen;
    if (body > total - KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    if (attrs > total - KVSPACE_XVALUE_HEAD_LEN - body)
        return -1;
    if (h.storetype != KVSPACE_XVALUE_ST_VARLEN) {
        /* 非 4 出现 tag 2 = 有定义但在此形态下无意义，同样拒绝。 */
        const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + body;
        uint64_t left = attrs;
        while (left >= 3) {
            const uint8_t tag = p[0];
            const uint16_t rlen = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
            if ((uint64_t)rlen + 3 > left)
                return -1;
            if (tag == KVSPACE_XVALUE_ATTR_TAG_REALLEN)
                return -1;
            p += 3 + rlen;
            left -= (uint64_t)rlen + 3;
        }
        /* 尾部不足一条记录的残余字节**忽略**——与 Rust 侧 `while let [tag,l0,l1,..]`
         * 同语义。历史 wire（`test_abi_contract` 的 padded 用例）就是
         * `attrslen=2` 的 `{0,0}`：既不是完整记录，也不该判畸形。 */
        *out = body;
        return 0;
    }
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + body;
    uint64_t left = attrs;
    int seen = 0;
    uint64_t reallen = 0;
    while (left >= 3) {
        const uint8_t tag = p[0];
        const uint16_t rlen = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if ((uint64_t)rlen + 3 > left)
            return -1;
        if (tag == KVSPACE_XVALUE_ATTR_TAG_REALLEN) {
            if (seen || rlen != 8)
                return -1;
            uint64_t v = 0;
            for (int i = 0; i < 8; i++)
                v |= (uint64_t)p[3 + i] << (8 * i);
            reallen = v;
            seen = 1;
        }
        p += 3 + rlen;
        left -= (uint64_t)rlen + 3;
    }
    if (!seen || reallen > body)
        return -1;
    *out = reallen;
    return 0;
}

#endif
