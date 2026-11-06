#include <kvspace/xvalue_head.h>
#include "xvalue_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 固定 head 16 字节 [bodylen:u64][attrslen:u32@8][layout@12][padding@13,16)，
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
    /* CAPACITY 布局在 langtype 记录之后**多一条 reallen 记录**（11 字节）。 */
    const uint64_t extra = h.layout == KVSPACE_XVALUE_LAYOUT_CAPACITY ? 11U : 0U;
    if (l == 0)
        return alen == extra;
    if (alen != 3 + l + extra)
        return 0;
    return a[0] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[1] == l && a[2] == 0 &&
           memcmp(a + 3, lt, l) == 0;
}

/* layout 落在 @12、padding 在 @13-15 且必须为零。 */
static int head_ok(const uint8_t *v, uint8_t layout) {
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    return kvspaceXvalueHeadPaddingIsZero(&h) && h.layout == layout &&
           kvspaceXvalueHeadLayoutIsValid(&h) && v[12] == layout && v[13] == 0 &&
           v[14] == 0 && v[15] == 0;
}

static void test_none(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&buf, &n) == 0, "none new");
    CHECK(n == 16, "none len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH &&
          kvspaceXvalueHeadLen(&h) == 0, "none head");
    /* wire 布局：bodylen@0-7、attrslen@8-11、layout@12、padding@13-15 */
    CHECK(buf[0] == 0 && buf[7] == 0 && buf[8] == 0 && buf[11] == 0,
          "none wire layout");
    CHECK(attr_ok(buf, n, 0, ""), "none empty attr");
    free(buf);
}

static void test_scalar(void) {
    uint8_t raw[8] = {0x0a, 0, 0, 0, 0, 0, 0, 0};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "int64",
                                     &buf, &n) == 0, "i64 new");
    CHECK(n == 16 + 8 + 8, "i64 len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8 &&
          h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH, "i64 head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0, "i64 body@8");
    CHECK(attr_ok(buf, n, 8, "int64"), "i64 langtype attr");
    free(buf);
    /* 无类型（空 langtype）：空 attr [attr_len:2]。 */
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "",
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
    CHECK(kvspaceXvalueHeadLen(&h) == 2 &&
          h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH, "slack head");
    /* wire：bodylen@0=2、attrslen@8=3+10、layout@12、padding@13-15 */
    CHECK(buf[0] == 2 && buf[1] == 0 && buf[7] == 0 &&
          head_ok(buf, KVSPACE_XVALUE_LAYOUT_LENGTH),
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
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH &&
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
    /* NewPtr 给 langtype 补 `*`：记录长度是 3 + 6（"*int64"）。 */
    CHECK(n == 16 + 5 + (3 + 6), "ptr len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 5, "ptr head");
    /* 引用性在 langtype 前缀，不在 head：NewPtr 给 langtype 补 `*`。 */
    CHECK(kvspace_c_reference_of(buf, n) == KVSPACE_XVALUE_REFERENCE_PTR,
          "ptr reference");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, "/varx", 5) == 0, "ptr body");
    CHECK(attr_ok(buf, n, 5, "*int64"), "ptr langtype attr (prefixed)");
    free(buf);

    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, "",
                                  &buf, &n) == 0, "ext new");
    CHECK(kvspace_c_reference_of(buf, n) == KVSPACE_XVALUE_REFERENCE_EXT,
          "ext reference");
    /* 空 langtype + NewExt ⇒ 裸 "@"（构造器补前缀）。 */
    CHECK(attr_ok(buf, n, 5, "@"), "ext langtype is bare @");
    free(buf);
    /* `path_len == 0` 是**合法**的（新实现只在 `path_len && !path` 时拒）——
     * 旧用例断言「empty ptr 失败」，那是旧实现的接口约定，不是格式的必然。 */
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 0, "", &buf, &n) == 0, "empty ptr is legal");
    free(buf);
    /* 已带前缀的 langtype 被拒绝（双重前缀说明两处对格式理解不一致）。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/varx", 5, "*int64", &buf, &n) != 0,
          "pre-prefixed langtype rejected");
}

static void test_reserve_code(void) {
    uint8_t *value = NULL;
    uint64_t len = 0;
    CHECK(kvspaceXvalueHeadReserve(7, 10, "[]u8",
                                   &value, &len) == 0, "reserve slack");
    /* cap > len ⇒ LAYOUT_CAPACITY：**body 区是 cap=10**，有效长度 7 记在 attr tag 2。 */
    CHECK(len == 16 + 10 + (3 + 4) + 11, "reserve len");
    kvspaceXvalueHead h;
    memcpy(&h, value, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 10, "reserve head len is capacity");
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_CAPACITY, "reserve layout capacity");
    {
        uint64_t effective = 0;
        CHECK(kvspace_c_effective_len(value, len, &effective) == 0 && effective == 7,
              "reserve effective len is the requested body length");
    }
    CHECK(attr_ok(value, len, 10, "[]u8"), "reserve langtype attr");
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    memcpy(value + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8);
    free(value);
    CHECK(kvspaceXvalueHeadReserve(10, 5, "",
                                   &value, &len) != 0, "reserve body>cap");

    const uint8_t call_body[] = {0, 0, 0, 0, 0, 'f'};
    CHECK(kvspaceXvalueHeadNewCode(call_body, sizeof call_body, sizeof call_body,
                                   "rwir", &value, &len) == 0, "code new");
    memcpy(&h, value, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == sizeof call_body, "code head");
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, call_body,
                 sizeof call_body) == 0, "code body");
    CHECK(attr_ok(value, len, sizeof call_body, "rwir"), "code langtype attr");
    free(value);
    CHECK(kvspaceXvalueHeadNewCode(call_body, 5, 3, "", &value, &len) != 0,
          "code cap<len");
}

/* Failure contract (kvspace fix/review-v0.3.23): return -1 and clear *out on
   failure; Reserve validates content; bounded langtype reader never
   reads past wire_len. */
static void test_fail_contract(void) {
    uint8_t sentinel = 0;
    uint8_t *buf = &sentinel;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(NULL, 3, "", &buf,
                                     &n) != 0,
          "NewScalar(NULL, 3) rejected");
    CHECK(buf == NULL, "NewScalar(NULL, 3) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewSlack(NULL, 4, 4, "", &buf, &n) != 0 && buf == NULL,
          "NewSlack(NULL, 4) rejected, *out cleared");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(8, 4, "", &buf, &n) != 0 && buf == NULL,
          "Reserve(cap < len) rejected, *out cleared");
    buf = NULL;
    CHECK(kvspaceXvalueHeadReserve(4, 4, "int32", &buf, &n) == 0 && buf,
          "Reserve ok");
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
/* 造一条 `layout == CAPACITY` 的 wire（body 区 cap 字节、有效 reallen，
   带 langtype 记录）。构造函数收 (body_len, cap)，这里要「容量已分配、有效长度
   更小」，`Reserve(cap, cap, …)` 出来的是 cap==len 的 LENGTH 布局，所以按格式
   手工拼，正好也验证读侧能独立解析。 */
static uint8_t *make_varlen(uint64_t cap, uint64_t reallen, const char *lt,
                            uint64_t *out_total) {
    size_t l = lt ? strlen(lt) : 0;
    uint64_t alen = (l ? 3 + l : 0) + 11; /* langtype 记录 + tag2 记录 */
    uint64_t total = KVSPACE_XVALUE_HEAD_LEN + cap + alen;
    uint8_t *v = (uint8_t *)calloc(1, total);
    kvspaceXvalueHead h = {.layout = KVSPACE_XVALUE_LAYOUT_CAPACITY,
                           .padding = {0u, 0u, 0u}};
    kvspaceXvalueHeadSetLen(&h, cap);
    kvspaceXvalueHeadSetAttrsLen(&h, (uint32_t)alen);
    memcpy(v, &h, sizeof h);
    uint8_t *a = v + KVSPACE_XVALUE_HEAD_LEN + cap;
    if (l) {
        a[0] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
        a[1] = (uint8_t)l; a[2] = (uint8_t)(l >> 8);
        memcpy(a + 3, lt, l);
        a += 3 + l;
    }
    a[0] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
    a[1] = 8; a[2] = 0;
    for (int i = 0; i < 8; i++)
        a[3 + i] = (uint8_t)(reallen >> (8 * i));
    *out_total = total;
    return v;
}

static void test_varlen(void) {
    uint64_t total = 0;
    /* 闭环：容量 32、有效 5；有效区之外是容量余量，不能被当成数据。 */
    uint8_t *v = make_varlen(32, 5, "char/utf8", &total);
    uint64_t eff = 123;
    CHECK(kvspace_c_effective_len(v, total, &eff) == 0, "varlen effective ok");
    CHECK(eff == 5, "effective=%llu expect 5", (unsigned long long)eff);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 32, "head len is capacity");
    const uint8_t *lt = NULL;
    CHECK(kvspaceXvalueHeadLangtypeBounded(v, total, &lt) == 9, "langtype record");
    free(v);

    /* reallen == 0 合法：容量已分配、当前无内容。 */
    v = make_varlen(16, 0, NULL, &total);
    eff = 123;
    CHECK(kvspace_c_effective_len(v, total, &eff) == 0 && eff == 0, "reallen=0 legal");
    free(v);

    /* 拒绝①：CAPACITY 布局缺 tag 2（把 attrslen 改成 0，等于没有 tag 2）。 */
    v = make_varlen(8, 3, NULL, &total);
    memcpy(&h, v, sizeof h);
    kvspaceXvalueHeadSetAttrsLen(&h, 0);
    memcpy(v, &h, sizeof h);
    CHECK(kvspace_c_effective_len(v, total, &eff) != 0, "missing tag2 rejected");
    free(v);

    /* 拒绝②：reallen > 容量。 */
    v = make_varlen(8, 9, NULL, &total);
    CHECK(kvspace_c_effective_len(v, total, &eff) != 0, "reallen>cap rejected");
    free(v);

    /* 拒绝③：非 CAPACITY 布局带 tag 2。 */
    {
        uint8_t *w = make_varlen(8, 3, NULL, &total);
        kvspaceXvalueHead h2;
        memcpy(&h2, w, sizeof h2);
        h2.layout = KVSPACE_XVALUE_LAYOUT_LENGTH;
        memcpy(w, &h2, sizeof h2);
        CHECK(kvspace_c_effective_len(w, total, &eff) != 0,
              "tag2 under non-varlen rejected");
        free(w);
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
    test_varlen();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
