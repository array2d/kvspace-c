/* 原地写契约：只允许改 body [8, 8+len)，head 与 attr 必须逐字节不变；
   改 head/attr 或改 body 长度的入口一律硬错。各入口用独立键，互不阻断。 */
#include <sys/types.h>

#include "test_util.h"

#include <kvspace/kvspace.h>
#include <kvspace/xvalue_head.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

static int head_attr_same(const struct Value *a, const struct Value *b) {
    return a->len == b->len && memcmp(a->raw, b->raw, 8) == 0 &&
           memcmp(a->raw + 16, b->raw + 16, a->len - 16) == 0;
}

static void run(void) {
    char dir[] = "/tmp/kvspace-inplace-XXXXXX";
    char *made = mkdtemp(dir);
    REQUIRE(made != NULL, "mkdtemp");
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);
    void *h = kvspaceConnect(dsn);
    REQUIRE(h != NULL, "connect");

    /* 1. ViewWrite 改长度：拒绝且不改值；等长接受，head/attr 不变。 */
    put_i64(h, "/vw");
    struct Value vw0;
    snap(h, "/vw", &vw0);
    REQUIRE(vw0.raw != NULL && vw0.len == 26, "base /vw");
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
        CHECK(memcmp(vw1.raw + 8, vw0.raw + 8, 8) == 0,
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
            CHECK(*(int64_t *)(vw2.raw + 8) == 5, "ViewWrite body value");
            free(vw2.raw);
        }
    }

    /* 2. WriteInPlace 改 body 长度：硬错；等长接受，head/attr 不变。 */
    put_i64(h, "/wi");
    struct Value wi0;
    snap(h, "/wi", &wi0);
    REQUIRE(wi0.raw != NULL && wi0.len == 26, "base /wi");
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
        CHECK(memcmp(wi1.raw, wi0.raw, 8) == 0, "WriteInPlace changed head");
        CHECK(memcmp(wi1.raw + 16, wi0.raw + 16, wi0.len - 16) == 0,
              "WriteInPlace changed attr");
        CHECK(*(int64_t *)(wi1.raw + 8) == 7, "WriteInPlace body value");
        free(wi1.raw);
    }

    /* 3. SetPart 写 head：硬错，整值不变。 */
    put_i64(h, "/sp");
    struct Value sp0;
    snap(h, "/sp", &sp0);
    REQUIRE(sp0.raw != NULL && sp0.len == 26, "base /sp");
    uint8_t patch[8] = {0xde, 0xad, 0xbe, 0xef, 0, 0, 0, 0};
    char err[128] = {0};
    const int sp_rc = kvspaceSetPart(h, "/sp", 0, patch, 8, err, sizeof err);
    CHECK(sp_rc != 0, "SetPart(offset=0) must fail");
    struct Value sp1;
    snap(h, "/sp", &sp1);
    CHECK(sp1.raw != NULL, "value intact after rejected head write");
    if (sp1.raw)
        CHECK(sp1.len == sp0.len && memcmp(sp1.raw, sp0.raw, sp0.len) == 0,
              "head write changed value");

    /* 4. SetPart 写 body：接受；head/attr 逐字节不变，body 变。 */
    if (sp1.raw) {
        int64_t nine = 99;
        CHECK(kvspaceSetPart(h, "/sp", 8, (const uint8_t *)&nine, 8, NULL, 0) ==
                  0,
              "SetPart(body) must succeed");
        struct Value sp2;
        snap(h, "/sp", &sp2);
        REQUIRE(sp2.raw != NULL && sp2.len == sp0.len, "body write geometry");
        CHECK(memcmp(sp2.raw, sp0.raw, 8) == 0, "body write changed head");
        CHECK(memcmp(sp2.raw + 16, sp0.raw + 16, sp0.len - 16) == 0,
              "body write changed attr");
        CHECK(*(int64_t *)(sp2.raw + 8) == 99, "body write body value");
        free(sp2.raw);
    }

    free(vw0.raw); free(vw1.raw);
    free(wi0.raw);
    free(sp0.raw); free(sp1.raw);
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
