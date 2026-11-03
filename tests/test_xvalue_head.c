#include "xvalue_head.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 固定 head 16 字节 [bodylen:u64][attrslen:u32@8][storetype@12][ptr@13][pad@14,16)，
   body 偏移恒 16，attr 区紧随 body 且**没有**长度前缀（attrslen 在 head 里）。
   单 key 整体——无 attr key。 */

static int failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

/* 校验 attr 区：total = 16+bodylen+attrslen；attr 区没有长度前缀，记录形状仍是
   [tag:u8][len:u16][data]。 */
static int attr_ok(const uint8_t *v, uint64_t total, uint64_t body_len,
                   const char *lt) {
    size_t l = strlen(lt);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    const uint8_t *a = kvspaceXvalueHeadAttrs(v);
    const uint64_t alen = (uint64_t)h.attrslen;
    if (total != kvspaceXvalueHeadTotalLen(&h))
        return 0;
    if (l == 0)
        return alen == 0;
    if (alen != 3 + l)
        return 0;
    return a[0] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[1] == l && a[2] == 0 &&
           memcmp(a + 3, lt, l) == 0;
}

/* padding 必须为零，storetype/ptr 必须落在 wire 的固定偏移上。 */
static int head_ok(const uint8_t *v, uint8_t storetype, uint8_t ptr) {
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    return kvspaceXvalueHeadPaddingIsZero(&h) && h.storetype == storetype &&
           h.ptr == ptr && v[12] == storetype && v[13] == ptr && v[14] == 0 &&
           v[15] == 0;
}

static void test_none(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&buf, &n) == 0, "none new");
    CHECK(n == 16, "none len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT &&
          h.ptr == KVSPACE_XVALUE_PTR_INLINE && kvspaceXvalueHeadLen(&h) == 0, "none head");
    /* wire 布局：bodylen@0-7、attrslen@8-11、storetype@12、ptr@13、pad@14-15 */
    CHECK(buf[0] == 0 && buf[7] == 0 && buf[8] == 0 && buf[11] == 0,
          "none wire layout");
    CHECK(attr_ok(buf, n, 0, ""), "none empty attr");
    free(buf);
}

static void test_scalar(void) {
    uint8_t raw[8] = {0x0a, 0, 0, 0, 0, 0, 0, 0};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "int64",
                                     &buf, &n) == 0, "i64 new");
    CHECK(n == 16 + 8 + 8, "i64 len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8 && h.storetype == KVSPACE_XVALUE_ST_SHORT, "i64 head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0, "i64 body@8");
    CHECK(attr_ok(buf, n, 8, "int64"), "i64 langtype attr");
    free(buf);
    /* 无类型（空 langtype）：空 attr [attr_len:2]。 */
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "",
                                     &buf, &n) == 0, "scalar no-langtype");
    CHECK(n == 16 + 8, "no-langtype len %llu", (unsigned long long)n);
    CHECK(attr_ok(buf, n, 8, ""), "no-langtype empty attr");
    free(buf);
}

static void test_slack(void) {
    const uint8_t hi[] = {'h', 'i'};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, 2, "[]char/utf8", &buf, &n) == 0, "slack new");
    CHECK(n == 16 + 8 + 8, "slack len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&h) == 2, "slack head");
    /* wire：bodylen@0=2、attrslen@8=3+10、storetype@12、ptr@13、pad@14-15 */
    CHECK(buf[0] == 2 && buf[1] == 0 && buf[7] == 0 &&
          head_ok(buf, KVSPACE_XVALUE_ST_SLACK, KVSPACE_XVALUE_PTR_INLINE),
          "slack wire layout");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, hi, 2) == 0, "slack body");
    CHECK(attr_ok(buf, n, 2, "[]char/utf8"), "slack langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, 1, "", &buf, &n) != 0, "slack cap<len");
}

static void test_tensor(void) {
    uint64_t dims[2] = {2, 3};
    uint8_t raw[24];
    memset(raw, 0xab, sizeof raw);
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, raw, sizeof raw, "[2,3]int64",
                                     &buf, &n) == 0, "tensor new");
    CHECK(n == 16 + 24 + (3 + 10), "tensor len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_LARGE &&
          kvspaceXvalueHeadLen(&h) == 24, "tensor head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 24) == 0, "tensor body@16");
    CHECK(attr_ok(buf, n, 24, "[2,3]int64"), "tensor langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewTensor(NULL, 0, raw, 24, "", &buf, &n) != 0,
          "tensor no dims");
}

static void test_ptr_ext(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/varx", 5, "int64",
                                  &buf, &n) == 0, "ptr new");
    CHECK(n == 16 + 5 + (3 + 5), "ptr len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          h.ptr == KVSPACE_XVALUE_PTR_PTR &&
          kvspaceXvalueHeadLen(&h) == 5, "ptr head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, "/varx", 5) == 0, "ptr body");
    CHECK(attr_ok(buf, n, 5, "int64"), "ptr langtype attr");
    free(buf);

    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, "",
                                  &buf, &n) == 0, "ext new");
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_EXT &&
          h.ptr == KVSPACE_XVALUE_PTR_EXT, "ext head");
    CHECK(attr_ok(buf, n, 5, ""), "ext empty attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 0, "", &buf, &n) != 0, "empty ptr");
}

static void test_reserve_code(void) {
    uint8_t *value = NULL;
    uint64_t len = 0;
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 7, 10, "[]u8",
                                   &value, &len) == 0, "reserve slack");
    CHECK(len == 16 + 7 + (3 + 4), "reserve len");
    kvspaceXvalueHead h;
    memcpy(&h, value, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 7, "reserve head len");
    CHECK(attr_ok(value, len, 7, "[]u8"), "reserve langtype attr");
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    memcpy(value + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8);
    free(value);
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_LARGE, 10, 5, "",
                                   &value, &len) != 0, "reserve body>cap");

    const uint8_t call_body[] = {0, 0, 0, 0, 0, 'f'};
    CHECK(kvspaceXvalueHeadNewCode(call_body, sizeof call_body, sizeof call_body,
                                   "rwir", &value, &len) == 0, "code new");
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&h) == sizeof call_body,
          "code head");
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, call_body,
                 sizeof call_body) == 0, "code body");
    CHECK(attr_ok(value, len, sizeof call_body, "rwir"), "code langtype attr");
    free(value);
    CHECK(kvspaceXvalueHeadNewCode(call_body, 5, 3, "", &value, &len) != 0,
          "code cap<len");
}

/* Failure contract (kvspace fix/review-v0.3.23): return -1 and clear *out on
   failure; Reserve validates storetype via build; bounded langtype reader never
   reads past wire_len. */
static void test_fail_contract(void) {
    uint8_t sentinel = 0;
    uint8_t *buf = &sentinel;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, NULL, 3, "", &buf,
                                     &n) != 0,
          "NewScalar(NULL, 3) rejected");
    CHECK(buf == NULL, "NewScalar(NULL, 3) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewSlack(NULL, 4, 4, "", &buf, &n) != 0 && buf == NULL,
          "NewSlack(NULL, 4) rejected, *out cleared");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(9, 4, 4, "", &buf, &n) != 0,
          "Reserve(storetype=9) rejected");
    CHECK(buf == NULL, "Reserve(storetype=9) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 8, 4, "", &buf, &n) != 0 &&
              buf == NULL,
          "Reserve(cap < len) rejected, *out cleared");
    buf = NULL;
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 4, 4, "int32", &buf, &n) ==
                  0 && buf,
          "Reserve(SLACK) ok");
    if (buf) {
        const uint8_t *lt = NULL;
        CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 5 && lt &&
                  memcmp(lt, "int32", 5) == 0,
              "bounded langtype reads full value");
        CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n - 1, &lt) == 0 && lt == NULL,
              "bounded langtype rejects truncated value");
        CHECK(kvspaceXvalueHeadLangtypeBounded(buf, KVSPACE_XVALUE_HEAD_LEN + 4, &lt) == 0,
              "bounded langtype rejects value without attr");
        free(buf);
    }
}
int main(void) {
    test_none();
    test_scalar();
    test_slack();
    test_tensor();
    test_ptr_ext();
    test_reserve_code();
    test_fail_contract();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
