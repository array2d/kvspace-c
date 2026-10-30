#define _GNU_SOURCE
#include <kvspace/kvspace.h>
#include "xvalue_head.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

/* #566：head 固定 8 字节 [len:u48][storetype][ptr]，body 偏移恒 8。直接 memcpy
   kvspaceXvalueHead + 字段访问。langtype 不在 head（x‥langtype attr key）。
   测试保留的 ABI New* 构造器产物 + 固定头字段。 */

/* 读固定头：返回 (storetype, ptr, body_len)，并校验总长。 */
static int peek(const uint8_t *v, uint32_t total, uint8_t *st, uint8_t *ptr,
                uint64_t *blen) {
    if (total < KVSPACE_XVALUE_HEAD_LEN)
        return 1;
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    if (h.storetype > KVSPACE_XVALUE_ST_EXT || h.ptr > KVSPACE_XVALUE_PTR_EXT)
        return 1;
    uint64_t l = kvspaceXvalueHeadLen(&h);
    if ((uint64_t)KVSPACE_XVALUE_HEAD_LEN + l != total)
        return 1;
    *st = h.storetype;
    *ptr = h.ptr;
    *blen = l;
    return 0;
}

static int test_head_constructors(void) {
    /* None：8 字节空体。 */
    uint8_t *v = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&v, &n) == 0 && n == 8);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT && h.ptr == KVSPACE_XVALUE_PTR_INLINE &&
          kvspaceXvalueHeadLen(&h) == 0);
    free(v);

    /* 标量：int64 8 字节小端 body。 */
    uint8_t raw[8] = {42, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, &v, &n) == 0);
    CHECK(n == 16);
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8 && h.storetype == KVSPACE_XVALUE_ST_SHORT);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0);
    free(v);
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, NULL, 8, &v, &n) != 0);

    /* slack：变长 body。 */
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceXvalueHeadNewSlack(utf8, sizeof utf8, sizeof utf8, &v, &n) == 0);
    CHECK(n == 8 + sizeof utf8);
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == sizeof utf8 && h.storetype == KVSPACE_XVALUE_ST_SLACK);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    free(v);

    /* tensor：LARGE storetype，body=元素连续。 */
    uint64_t dims[2] = {2, 3};
    uint8_t tbody[24];
    memset(tbody, 0, sizeof tbody);
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, tbody, sizeof tbody, &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_LARGE && kvspaceXvalueHeadLen(&h) == 24);
    free(v);

    /* ptr / ext。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK && h.ptr == KVSPACE_XVALUE_PTR_PTR &&
          kvspaceXvalueHeadLen(&h) == 2);
    free(v);
    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, &v, &n) == 0);
    memcpy(&h, v, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_EXT && h.ptr == KVSPACE_XVALUE_PTR_EXT);
    free(v);
    return 0;
}

/* 保留的 ABI New* 构造器 → 固定头 + body 字段断言。 */
static int test_new_abi(void) {
    uint8_t *v = NULL;
    uint32_t len = 0;
    uint8_t st, ptr;
    uint64_t blen;

    CHECK(kvspaceNewInt64(42, &v, &len) == 0);
    CHECK(len == 16 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SHORT && ptr == KVSPACE_XVALUE_PTR_INLINE && blen == 8);
    CHECK(v[8] == 42 && v[9] == 0); /* 小端 */
    free(v);

    CHECK(kvspaceNewFloat64(1.5, &v, &len) == 0);
    CHECK(len == 16 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SHORT && blen == 8);
    free(v);

    CHECK(kvspaceNewBool(1, &v, &len) == 0);
    CHECK(len == 9 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SHORT && blen == 1 && v[8] == 1);
    free(v);

    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceNewChar(utf8, sizeof utf8, &v, &len) == 0);
    CHECK(len == 8 + sizeof utf8 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(st == KVSPACE_XVALUE_ST_SLACK && blen == sizeof utf8);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    free(v);

    CHECK(kvspaceNewPtr("int64", "/x", &v, &len) == 0);
    CHECK(len == 10 && peek(v, len, &st, &ptr, &blen) == 0);
    CHECK(ptr == KVSPACE_XVALUE_PTR_PTR && st == KVSPACE_XVALUE_ST_SLACK && blen == 2);
    CHECK(memcmp(v + KVSPACE_XVALUE_HEAD_LEN, "/x", 2) == 0);
    free(v);
    return 0;
}

/* #566：x‥langtype attr key 值为字符串 XValue（8 字节 head + body=明文类型串）。
   kvspace 层不写 attr key——由上层（测试在此模拟）先写 attr、后写主值；
   kvspaceGetHead 解 attr body 得明文。 */
static int test_gethead_attr(void) {
    char dir[] = "/tmp/kvspace-gethead-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);
    void *h = kvspaceConnect(dsn);
    CHECK(h != NULL);
    /* 上层写 attr（attr 先落、x 后落）：attr 值是字符串 XValue。 */
    uint8_t *lt = NULL;
    uint64_t ltn = 0;
    CHECK(kvspaceXvalueHeadNewSlack((const uint8_t *)"int64", 5, 5, &lt, &ltn) == 0);
    CHECK(kvspaceSetValue(h, "/x\xE2\x80\xA5langtype", lt, (uint32_t)ltn, NULL, 0) == 0);
    free(lt);
    uint8_t *body = NULL;
    CHECK(kvspaceWriteNewPlace(h, "/x", 0, KVSPACE_XVALUE_ST_SHORT, 8, 8,
                               &body, NULL, 0) == 0);
    /* attr 值是字符串 XValue：wire = 8 字节 head + body=明文串。 */
    lt = NULL;
    uint32_t ltlen = 0;
    CHECK(kvspaceGet(h, "/x\xE2\x80\xA5langtype", 0, &lt, &ltlen) == 0);
    CHECK(lt && ltlen == KVSPACE_XVALUE_HEAD_LEN + 5);
    kvspaceXvalueHead lth;
    memcpy(&lth, lt, sizeof lth);
    CHECK(lth.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&lth) == 5);
    CHECK(memcmp(lt + KVSPACE_XVALUE_HEAD_LEN, "int64", 5) == 0);
    /* GetHead 解 body 得明文类型串。 */
    kvspaceHead_t head;
    CHECK(kvspaceGetHead(h, "/x", &head) == 0);
    CHECK(head.langtype_len == 5 && strcmp(head.langtype, "int64") == 0);
    /* 无 attr 的普通值：GetHead 仍成功、langtype 为空。 */
    uint8_t *v = NULL;
    uint32_t vlen = 0;
    CHECK(kvspaceNewInt64(42, &v, &vlen) == 0);
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
        test_gethead_attr() != 0)
        return 1;
    puts("ok");
    return 0;
}
