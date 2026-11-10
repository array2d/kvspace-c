#define _GNU_SOURCE
#include <kvspace/kvspace.h>
#include <kvspace/xvalue_head.h>
#include "xvalue_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

/* head 固定 16 字节 [bodylen:u64][attrslen:u16][padding:6]，body 偏移恒 16，
   attr 区紧随 body 且没有长度前缀。整体单 key——无 attr key。 */

/* 读固定头：返回 body_len 与有效长度，并校验总长 ≥ head+body。
 *
 * 旧版返回 (storetype, ptr)；更早的一版返回 layout——三个字段现在都不在 head
 * 里：padding 由 `TotalLen` 一并校验，引用性由 langtype 前缀表达，「bodylen
 * 是不是容量」由 langtype 判。 */
static int peek(const uint8_t *v, uint32_t total, uint64_t *blen,
                uint64_t *effective) {
    if (total < KVSPACE_XVALUE_HEAD_LEN)
        return 1;
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    const uint64_t full = kvspaceXvalueHeadTotalLen(&h);
    if (full == 0 || full > total)
        return 1;
    if (kvspaceXvalueHeadEffectiveLen(v, total, effective) != 0)
        return 1;
    *blen = kvspaceXvalueHeadLen(&h);
    return 0;
}

/* 校验 attr 区：total = 16+bodylen+attrslen；langtype 记录存在且与 lt 一致。 */
static int attr_ok(const uint8_t *v, uint32_t total, uint64_t body_len,
                   const char *lt) {
    size_t l = strlen(lt);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    const uint8_t *a = kvspaceXvalueHeadAttrs(v);
    const uint64_t alen = (uint64_t)h.attrslen;
    if ((uint64_t)total != kvspaceXvalueHeadTotalLen(&h))
        return 0;
    if (l == 0)
        return alen == 0;
    if (alen != 3 + l)
        return 0;
    return a[0] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[1] == l && a[2] == 0 &&
           memcmp(a + 3, lt, l) == 0;
}

static int test_head_constructors(void) {
    /* None：16 字节空体 + 空 attr。 */
    uint8_t *v = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&v, &n) == 0 && n == 16);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 0 && kvspaceXvalueHeadPaddingIsZero(&h));
    CHECK(attr_ok(v, (uint32_t)n, 0, ""));
    free(v);

    /* 标量：int64 8 字节小端 body + attr。 */
    uint8_t raw[8] = {42, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "int64",
                                     &v, &n) == 0);
    CHECK(n == 16 + 8 + (3 + 5));
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0);
    CHECK(attr_ok(v, (uint32_t)n, 8, "int64"));
    free(v);
    CHECK(kvspaceXvalueHeadNewScalar(NULL, 8, "",
                                     &v, &n) != 0);
    /* 空 langtype：空 attr [attr_len:2]。 */
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "",
                                     &v, &n) == 0);
    CHECK(n == 16 + 8);
    CHECK(attr_ok(v, (uint32_t)n, 8, ""));
    free(v);

    /* slack：变长 body + 类型记录。 */
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceXvalueHeadNewSlack(utf8, sizeof utf8, "char/utf8",
                                    &v, &n) == 0);
    CHECK(n == 16 + sizeof utf8 + (3 + 9));
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == sizeof utf8);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    CHECK(attr_ok(v, (uint32_t)n, sizeof utf8, "char/utf8"));
    free(v);

    /* tensor：body=元素连续，形状在 langtype（新布局里没有「LARGE」这个字段）。 */
    uint64_t dims[2] = {2, 3};
    uint8_t tbody[24];
    memset(tbody, 0, sizeof tbody);
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, tbody, sizeof tbody, "[2,3]int64",
                                     &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 24);
    CHECK(attr_ok(v, (uint32_t)n, 24, "[2,3]int64"));
    free(v);

    /* ptr / ext。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, "int64",
                                  &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(kvspace_c_reference_of(v, n) == KVSPACE_XVALUE_REFERENCE_PTR &&
          kvspaceXvalueHeadLen(&h) == 2);
    CHECK(attr_ok(v, (uint32_t)n, 2, "*int64"));
    free(v);
    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, "",
                                  &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(kvspace_c_reference_of(v, n) == KVSPACE_XVALUE_REFERENCE_EXT);
    CHECK(attr_ok(v, (uint32_t)n, 5, "@"));
    free(v);
    return 0;
}

/* 保留的 ABI New* 构造器 → 固定头 + body + attr 断言。 */
static int test_new_abi(void) {
    uint8_t *v = NULL;
    uint32_t len = 0;
    uint64_t blen;
    uint64_t effective;

    CHECK(kvspaceNewInt64(42, "int64", &v, &len) == 0);
    CHECK(len == 16 + 8 + (3 + 5) && peek(v, len, &blen, &effective) == 0);
    CHECK(blen == 8 && effective == 8);
    CHECK(v[KVSPACE_XVALUE_HEAD_LEN] == 42 && v[KVSPACE_XVALUE_HEAD_LEN + 1] == 0); /* 小端 */
    CHECK(attr_ok(v, len, 8, "int64"));
    free(v);

    CHECK(kvspaceNewFloat64(1.5, "float64", &v, &len) == 0);
    CHECK(len == 16 + 8 + (3 + 7) && peek(v, len, &blen, &effective) == 0);
    CHECK(blen == 8 && effective == 8);
    CHECK(attr_ok(v, len, 8, "float64"));
    free(v);

    CHECK(kvspaceNewBool(1, "bool", &v, &len) == 0);
    CHECK(len == 16 + 8 && peek(v, len, &blen, &effective) == 0);
    CHECK(blen == 1 && effective == 1 && v[KVSPACE_XVALUE_HEAD_LEN] == 1);
    CHECK(attr_ok(v, len, 1, "bool"));
    free(v);

    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceNewChar(utf8, sizeof utf8, "char/utf8", &v, &len) == 0);
    CHECK(len == 16 + sizeof utf8 + (3 + 9) && peek(v, len, &blen, &effective) == 0);
    CHECK(blen == sizeof utf8 && effective == sizeof utf8);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    CHECK(attr_ok(v, len, sizeof utf8, "char/utf8"));
    free(v);

    CHECK(kvspaceNewPtr("int64", "/x", &v, &len) == 0);
    /* NewPtr 给 langtype 补 `*`：记录是 3 + 6（"*int64"），不是 3 + 5。 */
    CHECK(len == 16 + 2 + (3 + 6) && peek(v, len, &blen, &effective) == 0);
    CHECK(kvspace_c_reference_of(v, len) == KVSPACE_XVALUE_REFERENCE_PTR && blen == 2);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, "/x", 2) == 0);
    CHECK(attr_ok(v, len, 2, "*int64"));
    free(v);
    return 0;
}

/* #571：langtype 在 attr 区——WriteNewPlace 带类型整体单写，GetHead 读 attr 记录；
   无类型值 langtype 空。无 attr key。 */
/*
 * 两条等价判据：值里有没有这条 langtype、有效长度是多少。
 *
 * 都从 `kvspaceGet` 拿整条 wire 再用公开头的 head 解析——**不再有第二份 head
 * 读取入口**（旧的本地 head API 已删）。
 */
static int value_langtype_is(void *h, const char *key, const char *expected) {
    uint8_t *wire = NULL;
    uint32_t wire_len = 0;
    const uint8_t *langtype = NULL;
    uint16_t langtype_len;

    if (kvspaceGet(h, key, 0, &wire, &wire_len) != 0 || wire == NULL)
        return 0;
    langtype_len = kvspaceXvalueHeadLangtypeBounded(wire, wire_len, &langtype);
    if (langtype_len == 0)
        return expected[0] == '\0';
    if (langtype == NULL || langtype_len != (uint16_t)strlen(expected))
        return 0;
    return memcmp(langtype, expected, langtype_len) == 0;
}

static int value_length_is(void *h, const char *key, uint64_t expected) {
    uint8_t *wire = NULL;
    uint32_t wire_len = 0;
    uint64_t effective = 0;

    if (kvspaceGet(h, key, 0, &wire, &wire_len) != 0 || wire == NULL)
        return 0;
    if (wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return 0;
    /* 长度取**有效长度** = bodylen（wire 里没有容量，也没有 tag 2）。 */
    return kvspace_c_effective_len(wire, wire_len, &effective) == 0 &&
           effective == expected;
}

static int test_gethead_tail(void) {
    char dir[] = "/tmp/kvspace-gethead-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);
    void *h = kvspaceConnect(dsn);
    CHECK(h != NULL);
    uint8_t *body = NULL;
    CHECK(kvspaceWriteNewPlace(h, "/x", 0, KVSPACE_STORETYPE_FIXED_SMALL, 8, 8, "int64",
                               &body, NULL, 0) == 0);
    memset(body, 42, 8);
    /*
     * 判据不变（**langtype 与长度都读得出来**），取数换成公开头的等价入口：
     * `kvspaceGet` 拿 wire → `EffectiveLen` / `LangtypeBounded`。
     */
    CHECK(value_langtype_is(h, "/x", "int64"));
    CHECK(value_length_is(h, "/x", 8));
    /* 无类型值：GetHead 成功、langtype 为空。 */
    uint8_t *v = NULL;
    uint32_t vlen = 0;
    CHECK(kvspaceNewInt64(42, "", &v, &vlen) == 0);
    CHECK(kvspaceSetValue(h, "/y", v, vlen, NULL, 0) == 0);
    free(v);
    /* 无类型值：读得到 wire，但 langtype 记录不存在（长度 0）。 */
    CHECK(value_langtype_is(h, "/y", ""));
    kvspaceClose(h);
    char extra[160];
    snprintf(extra, sizeof extra, "rm -rf %s", dir);
    CHECK(system(extra) == 0);
    return 0;
}

int main(void) {
    if (test_head_constructors() != 0 || test_new_abi() != 0 ||
        test_gethead_tail() != 0)
        return 1;
    puts("ok");
    return 0;
}
