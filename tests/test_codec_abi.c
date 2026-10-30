#define _GNU_SOURCE
#include <kvspace/kvspace.h>
#include "xvalue_head.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

/* #571：head 固定 8 字节 [len:u48][storetype][ptr]，body 偏移恒 8，
   attr 区 @8+len（attr_len:u16 自描述，含 langtype 记录）。整体单 key——无 attr key。 */

/* 读固定头：返回 (storetype, ptr, body_len)，并校验总长 ≥ head+body。 */
static int peek(const uint8_t *v, uint32_t total, uint8_t *st, uint8_t *ptr,
                uint64_t *blen) {
    if (total < KVSPACE_XVALUE_HEAD_LEN)
        return 1;
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    if (h.storetype > KVSPACE_XVALUE_ST_EXT || h.ptr > KVSPACE_XVALUE_PTR_EXT)
        return 1;
    uint64_t l = kvspaceXvalueHeadLen(&h);
    if ((uint64_t)KVSPACE_XVALUE_HEAD_LEN + l > total)
        return 1;
    *st = h.storetype;
    *ptr = h.ptr;
    *blen = l;
    return 0;
}

/* 校验 attr 区：total = 8+body_len+attr_len；langtype 记录存在且与 lt 一致。 */
static int attr_ok(const uint8_t *v, uint32_t total, uint64_t body_len,
                   const char *lt) {
    size_t l = strlen(lt);
    const uint8_t *a = v + KVSPACE_XVALUE_HEAD_LEN + body_len;
    uint64_t alen = (uint64_t)a[0] | ((uint64_t)a[1] << 8);
    if ((uint64_t)total != KVSPACE_XVALUE_HEAD_LEN + body_len + alen)
        return 0;
    if (l == 0)
        return alen == 2;
    if (alen != 2 + 3 + l)
        return 0;
    return a[2] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[3] == l && a[4] == 0 &&
           memcmp(a + 5, lt, l) == 0;
}

static int test_head_constructors(void) {
    /* None：8 字节空体 + 空 attr。 */
    uint8_t *v = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&v, &n) == 0 && n == 10);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT && h.ptr == KVSPACE_XVALUE_PTR_INLINE &&
          kvspaceXvalueHeadLen(&h) == 0);
    CHECK(attr_ok(v, (uint32_t)n, 0, ""));
    free(v);

    /* 标量：int64 8 字节小端 body + attr。 */
    uint8_t raw[8] = {42, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "int64",
                                     &v, &n) == 0);
    CHECK(n == 26);
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8 && h.storetype == KVSPACE_XVALUE_ST_SHORT);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0);
    CHECK(attr_ok(v, (uint32_t)n, 8, "int64"));
    free(v);
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, NULL, 8, "",
                                     &v, &n) != 0);
    /* 空 langtype：空 attr [attr_len:2]。 */
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "",
                                     &v, &n) == 0);
    CHECK(n == 18);
    CHECK(attr_ok(v, (uint32_t)n, 8, ""));
    free(v);

    /* slack：变长 body + 类型记录。 */
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceXvalueHeadNewSlack(utf8, sizeof utf8, sizeof utf8, "char/utf8",
                                    &v, &n) == 0);
    CHECK(n == 8 + sizeof utf8 + 14);
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == sizeof utf8 && h.storetype == KVSPACE_XVALUE_ST_SLACK);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    CHECK(attr_ok(v, (uint32_t)n, sizeof utf8, "char/utf8"));
    free(v);

    /* tensor：LARGE storetype，body=元素连续，形状在 langtype。 */
    uint64_t dims[2] = {2, 3};
    uint8_t tbody[24];
    memset(tbody, 0, sizeof tbody);
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, tbody, sizeof tbody, "[2,3]int64",
                                     &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_LARGE && kvspaceXvalueHeadLen(&h) == 24);
    CHECK(attr_ok(v, (uint32_t)n, 24, "[2,3]int64"));
    free(v);

    /* ptr / ext。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, "int64",
                                  &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK && h.ptr == KVSPACE_XVALUE_PTR_PTR &&
          kvspaceXvalueHeadLen(&h) == 2);
    CHECK(attr_ok(v, (uint32_t)n, 2, "int64"));
    free(v);
    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, "",
                                  &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_EXT && h.ptr == KVSPACE_XVALUE_PTR_EXT);
    CHECK(attr_ok(v, (uint32_t)n, 5, ""));
    free(v);
    return 0;
}

/* 保留的 ABI New* 构造器 → 固定头 + body + attr 断言。 */
static int test_new_abi(void) {
    uint8_t *v = NULL;
    uint32_t len = 0;
    uint8_t st, ptr;
    uint64_t blen;

    CHECK(kvspaceNewInt64(42, "int64", &v, &len) == 0);
    CHECK(len == 26 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SHORT && ptr == KVSPACE_XVALUE_PTR_INLINE && blen == 8);
    CHECK(v[8] == 42 && v[9] == 0); /* 小端 */
    CHECK(attr_ok(v, len, 8, "int64"));
    free(v);

    CHECK(kvspaceNewFloat64(1.5, "float64", &v, &len) == 0);
    CHECK(len == 28 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SHORT && blen == 8);
    CHECK(attr_ok(v, len, 8, "float64"));
    free(v);

    CHECK(kvspaceNewBool(1, "bool", &v, &len) == 0);
    CHECK(len == 18 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SHORT && blen == 1 && v[8] == 1);
    CHECK(attr_ok(v, len, 1, "bool"));
    free(v);

    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceNewChar(utf8, sizeof utf8, "char/utf8", &v, &len) == 0);
    CHECK(len == 8 + sizeof utf8 + 14 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SLACK && blen == sizeof utf8);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    CHECK(attr_ok(v, len, sizeof utf8, "char/utf8"));
    free(v);

    CHECK(kvspaceNewPtr("int64", "/x", &v, &len) == 0);
    CHECK(len == 20 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(ptr == KVSPACE_XVALUE_PTR_PTR && st == KVSPACE_XVALUE_ST_SLACK && blen == 2);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, "/x", 2) == 0);
    CHECK(attr_ok(v, len, 2, "int64"));
    free(v);
    return 0;
}

/* #571：langtype 在 attr 区——WriteNewPlace 带类型整体单写，GetHead 读 attr 记录；
   无类型值 langtype 空。无 attr key。 */
static int test_gethead_tail(void) {
    char dir[] = "/tmp/kvspace-gethead-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);
    void *h = kvspaceConnect(dsn);
    CHECK(h != NULL);
    uint8_t *body = NULL;
    CHECK(kvspaceWriteNewPlace(h, "/x", 0, KVSPACE_XVALUE_ST_SHORT, 8, 8, "int64",
                               &body, NULL, 0) == 0);
    memset(body, 42, 8);
    kvspaceHead_t head;
    CHECK(kvspaceGetHead(h, "/x", &head) == 0);
    CHECK(head.langtype_len == 5 && strcmp(head.langtype, "int64") == 0);
    /* 无类型值：GetHead 成功、langtype 为空。 */
    uint8_t *v = NULL;
    uint32_t vlen = 0;
    CHECK(kvspaceNewInt64(42, "", &v, &vlen) == 0);
    CHECK(kvspaceSetValue(h, "/y", v, vlen, NULL, 0) == 0);
    free(v);
    CHECK(kvspaceGetHead(h, "/y", &head) == 0);
    CHECK(head.langtype_len == 0);
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
