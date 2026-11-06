/* 原地写契约：只允许改 body [8, 8+len)，head 与 attr 必须逐字节不变；
   改 head/attr 或改 body 长度的入口一律硬错。各入口用独立键，互不阻断。 */
#include <sys/types.h>

#include "test_util.h"

#include <kvspace/kvspace.h>
#include <kvspace/xvalue_head.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <string.h>

struct Value {
    uint8_t *raw;
    uint32_t len;
};

static void put_i64(void *h, const char *key) {
    uint8_t *wire = NULL;
    uint32_t wlen = 0;
    CHECK(kvspaceNewInt64(42, "int64", &wire, &wlen) == 0, "new int64");
    CHECK(kvspaceSetValue(h, key, wire, wlen, NULL, 0) == 0, "set %s", key);
    free(wire);
}

static void snap(void *h, const char *key, struct Value *out) {
    uint8_t *raw = NULL;
    out->len = 0;
    if (kvspaceGet(h, key, 0, &raw, &out->len) != 0 || !raw) {
        out->raw = NULL;
        return;
    }
    out->raw = malloc(out->len);
    memcpy(out->raw, raw, out->len);
}

/* head 与 attr 区不变；body 本来就是要改的那一段，不参与比较。
   attr 起点由各自的 bodylen 推出，所以逐字段取 head 后再比 body 之后的字节。 */
static int head_attr_same(const struct Value *a, const struct Value *b) {
    if (a->len != b->len ||
        memcmp(a->raw, b->raw, KVSPACE_XVALUE_HEAD_LEN) != 0)
        return 0;
    kvspaceXvalueHead ha;
    memcpy(&ha, a->raw, sizeof ha);
    const uint64_t body_a = kvspaceXvalueHeadLen(&ha);
    const uint64_t attrs = ha.attrslen;
    const uint8_t *attr_a = a->raw + KVSPACE_XVALUE_HEAD_LEN + body_a;
    const uint8_t *attr_b = b->raw + KVSPACE_XVALUE_HEAD_LEN + body_a;
    return memcmp(attr_a, attr_b, (size_t)attrs) == 0;
}

static void run(void) {
    /* 唯一前缀用标准库构造（time + 静态计数），不依赖 mkdtemp/开目录。
     *
     * 为什么不用 mkdtemp：它在 POSIX/BSD 命名空间，本仓 CMAKE_C_EXTENSIONS OFF
     * （严格 C11）。Linux 上补 _POSIX_C_SOURCE 即可；macOS 上试过
     * _POSIX_C_SOURCE / _DARWIN_C_SOURCE / _GNU_SOURCE 三种宏均未生效。
     * 原因未查明——同目录 test_art_alignment.c 用 _GNU_SOURCE + mkdtemp 在
     * macOS 上是能编过的，行为不一致。改为只依赖 C 标准库，两平台都不再需要
     * 特性宏；shm 后端自己创建文件，无需预先建目录。 */
    static unsigned seq = 0;
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm:///tmp/kvspace-inplace-%ld-%u.shm",
             (long)time(NULL), ++seq);
    void *h = kvspaceConnect(dsn);
    REQUIRE(h != NULL, "connect");

    /* 1. ViewWrite 改长度：拒绝且不改值；等长接受，head/attr 不变。 */
    put_i64(h, "/vw");
    struct Value vw0;
    snap(h, "/vw", &vw0);
    REQUIRE(vw0.raw != NULL && vw0.len == 16 + 8 + (3 + 5),
            "base /vw");
    const int qrc = kvspaceViewQuiesce(h, 0);
    CHECK(qrc == 0, "quiesce");
    kvspaceViewToken_t tok;
    memset(&tok, 0, sizeof tok);
    const int vrc = kvspaceViewResolve(h, "/vw", &tok);
    REQUIRE(vrc == 0, "view resolve");
    CHECK(tok.value_len == 8, "token value_len");
    uint8_t big[16];
    memset(big, 0xAA, sizeof big);
    const int vbad = kvspaceViewWrite(h, &tok, big, 16);
    CHECK(vbad != 0, "ViewWrite(len=16) must fail");
    struct Value vw1;
    snap(h, "/vw", &vw1);
    CHECK(vw1.raw != NULL, "rejected ViewWrite destroyed value");
    if (vw1.raw) {
        CHECK(vw1.len == vw0.len && head_attr_same(&vw1, &vw0),
              "rejected ViewWrite changed head/attr");
        CHECK(memcmp(vw1.raw + KVSPACE_XVALUE_HEAD_LEN,
                         vw0.raw + KVSPACE_XVALUE_HEAD_LEN, 8) == 0,
              "rejected ViewWrite changed body");
        int64_t five = 5;
        const int vok = kvspaceViewWrite(h, &tok, (const uint8_t *)&five, 8);
        CHECK(vok == 0, "ViewWrite(len=8)");
        struct Value vw2;
        snap(h, "/vw", &vw2);
        CHECK(vw2.raw != NULL && vw2.len == vw0.len && vw2.raw != NULL,
              "ViewWrite geometry");
        if (vw2.raw) {
            CHECK(head_attr_same(&vw2, &vw0), "ViewWrite changed head/attr");
            CHECK(*(int64_t *)(intptr_t)(vw2.raw + KVSPACE_XVALUE_HEAD_LEN) == 5,
                  "ViewWrite body value");
            free(vw2.raw);
        }
    }

    /* 2. WriteInPlace 改 body 长度：硬错；等长接受，head/attr 不变。 */
    put_i64(h, "/wi");
    struct Value wi0;
    snap(h, "/wi", &wi0);
    REQUIRE(wi0.raw != NULL && wi0.len == 16 + 8 + (3 + 5), "base /wi");
    uint8_t *body = NULL;
    const int wshort = kvspaceWriteInPlace(h, "/wi", 0, 4, &body, NULL, 0);
    CHECK(wshort != 0, "WriteInPlace(body_len=4) must fail");
    const int wok = kvspaceWriteInPlace(h, "/wi", 0, 8, &body, NULL, 0);
    CHECK(wok == 0, "WriteInPlace(body_len=8)");
    if (wok == 0) {
        int64_t seven = 7;
        memcpy(body, &seven, 8);
        struct Value wi1;
        snap(h, "/wi", &wi1);
        REQUIRE(wi1.raw != NULL && wi1.len == wi0.len, "in-place geometry");
        CHECK(memcmp(wi1.raw, wi0.raw, KVSPACE_XVALUE_HEAD_LEN) == 0,
              "WriteInPlace changed head");
        CHECK(head_attr_same(&wi1, &wi0), "WriteInPlace changed attr");
        CHECK(*(int64_t *)(intptr_t)(wi1.raw + KVSPACE_XVALUE_HEAD_LEN) == 7,
              "WriteInPlace body value");
        free(wi1.raw);
    }

    /* 3. SetPart 写 body：接受；head/attr 逐字节不变，body 变。 */
    put_i64(h, "/sp");
    struct Value sp0;
    snap(h, "/sp", &sp0);
    REQUIRE(sp0.raw != NULL && sp0.len == 16 + 8 + (3 + 5), "base /sp");
    {
        int64_t nine = 99;
        CHECK(kvspaceSetPart(h, "/sp", KVSPACE_XVALUE_HEAD_LEN,
                             (const uint8_t *)&nine, 8, NULL, 0) ==
                  0,
              "SetPart(body) must succeed");
        struct Value sp2;
        snap(h, "/sp", &sp2);
        REQUIRE(sp2.raw != NULL && sp2.len == sp0.len, "body write geometry");
        CHECK(memcmp(sp2.raw, sp0.raw, KVSPACE_XVALUE_HEAD_LEN) == 0,
              "body write changed head");
        CHECK(head_attr_same(&sp2, &sp0), "body write changed attr");
        CHECK(*(int64_t *)(intptr_t)(sp2.raw + KVSPACE_XVALUE_HEAD_LEN) == 99,
              "body write body value");
        free(sp2.raw);
    }

    free(vw0.raw); free(vw1.raw);
    free(wi0.raw);
    free(sp0.raw);
    kvspaceClose(h);
}

int main(void) {
    run();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    puts("inplace_body_only: ok");
    return 0;
}
