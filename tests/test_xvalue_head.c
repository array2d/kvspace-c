#include <kvspace/kvlang_xvalue.h>
#include "xvalue_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 固定 head 16 字节 [bodylen:u64@0][attrslen:u16@8][padding:6@10-15]，body 偏移
   恒 16，attr 区紧随 body 且**没有**长度前缀（attrslen 在 head 里）。单 key
   整体——无 attr key。bodylen 恒为有效字节数；容量是后端的物理分配属性，不进
   wire，所以也没有携带容量的 tag 2。 */

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
   [tag:u8][len:u16][data]。构造器只产 langtype 一条记录（tag 2 已删除）。 */
static int attr_ok(const uint8_t *v, uint64_t total, uint64_t body_len,
                   const char *lt) {
    size_t l = strlen(lt);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    const uint8_t *a = kvspaceXvalueHeadAttrs(v);
    const uint64_t alen = (uint64_t)h.attrslen;
    (void)body_len;
    if (total != kvspaceXvalueHeadTotalLen(&h))
        return 0;
    if (l == 0)
        return alen == 0;
    if (alen != 3 + l)
        return 0;
    return a[0] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[1] == l && a[2] == 0 &&
           memcmp(a + 3, lt, l) == 0;
}

/* 10-15 六字节 padding 必须全零；@12 曾是 `layout`，新布局里它只是 padding。 */
static int padding_zero(const uint8_t *v) {
    for (int i = 10; i < 16; i++)
        if (v[i] != 0)
            return 0;
    return 1;
}

static void test_none(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&buf, &n) == 0, "none new");
    CHECK(n == 16, "none len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 0, "none head");
    /* wire 布局：bodylen@0-7、attrslen@8-9、padding@10-15 */
    CHECK(buf[0] == 0 && buf[7] == 0 && buf[8] == 0 && buf[9] == 0 &&
          padding_zero(buf), "none wire layout");
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
    CHECK(kvspaceXvalueHeadLen(&h) == 8, "i64 head");
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
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, "[]char/utf8", &buf, &n) == 0, "slack new");
    CHECK(n == 16 + 8 + 8, "slack len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 2, "slack head");
    /* wire：bodylen@0=2、attrslen@8=3+10、padding@10-15 全零 */
    CHECK(buf[0] == 2 && buf[1] == 0 && buf[7] == 0 && padding_zero(buf),
          "slack wire layout");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, hi, 2) == 0, "slack body");
    CHECK(attr_ok(buf, n, 2, "[]char/utf8"), "slack langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewSlack(NULL, 2, "", &buf, &n) != 0, "slack NULL body");
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
    CHECK(kvspaceXvalueHeadLen(&h) == 24, "tensor head");
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
    /* Reserve 只产「bodylen 字节零 body + langtype 一条 attr」；容量是存储后端
       的物理分配属性，不进 wire。 */
    CHECK(kvspaceXvalueHeadReserve(4, "[]u8", &value, &len) == 0, "reserve");
    CHECK(len == 16 + 4 + 7, "reserve len %llu", (unsigned long long)len);
    kvspaceXvalueHead h;
    memcpy(&h, value, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 4, "reserve head len");
    CHECK(value[KVSPACE_XVALUE_HEAD_LEN] == 0 &&
          value[KVSPACE_XVALUE_HEAD_LEN + 3] == 0, "reserve body zero-filled");
    {
        uint64_t effective = 0;
        CHECK(kvspace_c_effective_len(value, len, &effective) == 0 && effective == 4,
              "reserve effective len is bodylen");
    }
    CHECK(attr_ok(value, len, 4, "[]u8"), "reserve langtype attr");
    /* wire 逐字节：bodylen=4、attrslen=7、padding 全零、attr = 01 04 00 5b 5d 75 38 */
    CHECK(value[0] == 4 && value[1] == 0 && value[7] == 0 && value[8] == 7 &&
          value[9] == 0 && padding_zero(value), "reserve wire head");
    CHECK(value[16 + 4] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE &&
          value[16 + 5] == 4 && value[16 + 6] == 0 &&
          memcmp(value + 16 + 7, "[]u8", 4) == 0, "reserve wire attr");
    free(value);

    const uint8_t call_body[] = {0, 0, 0, 0, 0, 'f'};
    CHECK(kvspaceXvalueHeadNewCode(call_body, sizeof call_body,
                                   "rwir", &value, &len) == 0, "code new");
    memcpy(&h, value, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == sizeof call_body, "code head");
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, call_body,
                 sizeof call_body) == 0, "code body");
    CHECK(attr_ok(value, len, sizeof call_body, "rwir"), "code langtype attr");
    free(value);
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
    CHECK(kvspaceXvalueHeadNewSlack(NULL, 4, "", &buf, &n) != 0 && buf == NULL,
          "NewSlack(NULL, 4) rejected, *out cleared");
    buf = NULL;
    CHECK(kvspaceXvalueHeadReserve(4, "int32", &buf, &n) == 0 && buf,
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
/* 造一条 body 区 body_len 字节、attr 区由调用方给的 wire。tag 2 已经删除，
   所以这里能造出「老格式必须被拒」的那些形状。 */
static uint8_t *make_wire(uint64_t body_len, const char *lt, uint16_t attr_len,
                          uint64_t *out_total) {
    size_t l = lt ? strlen(lt) : 0;
    uint64_t total = KVSPACE_XVALUE_HEAD_LEN + body_len + attr_len;
    uint8_t *v = (uint8_t *)calloc(1, (size_t)total);
    kvspaceXvalueHead h = {.padding = {0u, 0u, 0u, 0u, 0u, 0u}};
    kvspaceXvalueHeadSetLen(&h, body_len);
    kvspaceXvalueHeadSetAttrsLen(&h, attr_len);
    memcpy(v, &h, sizeof h);
    if (l && attr_len >= 3 + l) {
        uint8_t *a = kvspaceXvalueHeadAttrs(v) == NULL
                         ? v + KVSPACE_XVALUE_HEAD_LEN + body_len
                         : v + KVSPACE_XVALUE_HEAD_LEN + body_len;
        a[0] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
        a[1] = (uint8_t)l;
        a[2] = (uint8_t)(l >> 8);
        memcpy(a + 3, lt, l);
    }
    *out_total = total;
    return v;
}

/* 在 wire 的 attr 区尾部追加一条 tag 2（旧 reallen，11 字节），总长与 attrslen
   一并加长，返回同一条 wire 的 realloc 结果。 */
static uint8_t *append_reallen(uint8_t *v, uint64_t total, uint64_t body_len,
                               uint64_t reallen, uint64_t *out_total) {
    uint8_t *grown = (uint8_t *)realloc(v, (size_t)(total + 11));
    kvspaceXvalueHead h;
    memcpy(&h, grown, sizeof h);
    kvspaceXvalueHeadSetAttrsLen(&h, (uint16_t)(kvspaceXvalueHeadAttrsLen(&h) + 11));
    memcpy(grown, &h, sizeof h);
    uint8_t *a = grown + KVSPACE_XVALUE_HEAD_LEN + body_len +
                 kvspaceXvalueHeadAttrsLen(&h) - 11;
    a[0] = 2; /* 旧 KVSPACE_XVALUE_ATTR_TAG_REALLEN，常量已从上游删除 */
    a[1] = 8;
    a[2] = 0;
    for (int i = 0; i < 8; i++)
        a[3 + i] = (uint8_t)(reallen >> (8 * i));
    *out_total = total + 11;
    return grown;
}

static void test_reallen_rejected(void) {
    uint64_t total = 0;
    uint64_t eff = 123;
    const uint8_t *lt = NULL;
    uint16_t ltn = 0;

    /* `kvspace_c_scan_attrs` 是**拒绝 tag 2 的唯一闸口**（`EffectiveLen` 只读
       bodylen，不扫 attr；存储层 `head_of` 走这个扫描器，所以畸形在入场就被挡）。
       上游已删除 `kvspaceXvalueHeadScan`，`kvspaceXvalueGetAttr` 不能顶替它——
       后者找到目标 tag 就返回，不会继续扫后面的记录（见本文件末尾的对照用例）。 */

    /* 底线：只有 langtype 一条记录的 wire 通过，EffectiveLen == bodylen。 */
    {
        uint8_t *v = make_wire(8, "char/utf8", 3 + 9, &total);
        CHECK(kvspace_c_effective_len(v, total, &eff) == 0 && eff == 8,
              "effective len is bodylen");
        CHECK(kvspace_c_scan_attrs(v, total, &lt, &ltn) == 0 && ltn == 9,
              "baseline wire scans clean");
        free(v);
    }

    /* 拒绝①：出现 tag 2（旧 reallen）即拒。带 langtype。 */
    {
        uint8_t *v = make_wire(8, "char/utf8", 3 + 9, &total);
        v = append_reallen(v, total, 8, 5, &total);
        CHECK(kvspace_c_scan_attrs(v, total, &lt, &ltn) != 0,
              "tag2 after langtype rejected");
        free(v);
    }

    /* 拒绝②：tag 2 单独出现（无 langtype）也拒——它没有「合法位置」可言。 */
    {
        uint8_t *v = make_wire(8, NULL, 0, &total);
        v = append_reallen(v, total, 8, 5, &total);
        CHECK(kvspace_c_scan_attrs(v, total, &lt, &ltn) != 0,
              "tag2 alone rejected");
        free(v);
    }

    /* 拒绝③：两条 tag 2 也拒（第一条就够）。 */
    {
        uint8_t *v = make_wire(8, NULL, 0, &total);
        v = append_reallen(v, total, 8, 5, &total);
        v = append_reallen(v, total, 8, 5, &total);
        CHECK(kvspace_c_scan_attrs(v, total, &lt, &ltn) != 0,
              "two tag2 records rejected");
        free(v);
    }

    /* 拒绝④：padding 非零。@12 曾是 `layout` 字节——旧 wire 的 layout=1 就是
       非零 byte12，新布局必须按 padding 拒掉（这也是格式版本必须升的原因）。 */
    {
        for (int index = 10; index < 16; index++) {
            uint8_t *w = make_wire(8, NULL, 0, &total);
            w[index] = 1;
            CHECK(kvspace_c_effective_len(w, total, &eff) != 0,
                  "nonzero padding at byte %d rejected", index);
            CHECK(kvspace_c_scan_attrs(w, total, &lt, &ltn) != 0,
                  "nonzero padding at byte %d rejected by scan", index);
            free(w);
        }
        /* 反向对照：同一形状、padding 全零时通过，证明上面拒的是 padding 不是别的。 */
        uint8_t *w = make_wire(8, NULL, 0, &total);
        CHECK(kvspace_c_effective_len(w, total, &eff) == 0 && eff == 8,
              "same wire with zero padding accepted");
        free(w);
    }

    /* 已废 tag 2 的位置无关性：GetAttr 命中目标 tag 后也必须扫完整区，
     * 不能因为 tag 2 排在后面就放过畸形 wire。 */
    {
        const uint8_t *data = NULL;
        uint16_t dlen = 0;
        uint8_t *v = make_wire(8, "char/utf8", 3 + 9, &total);
        v = append_reallen(v, total, 8, 5, &total);
        CHECK(kvspaceXvalueGetAttr(v, total, KVSPACE_XVALUE_ATTR_TAG_LANGTYPE,
                                   &data, &dlen) != 0,
              "GetAttr 必须拒绝 tag1 之后的 tag2");
        CHECK(kvspace_c_scan_attrs(v, total, &lt, &ltn) != 0,
              "扫描器同样拒绝");
        free(v);
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
    test_reallen_rejected();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
