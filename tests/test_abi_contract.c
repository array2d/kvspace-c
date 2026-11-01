/* ABI contract regressions for v0.3.23 review findings:
 * 1) WriteNewPlace ref sets ptr byte (ref=1 link, ref=2 ext);
 * 2) WriteNewPlace body_cap reserves physical box capacity (view.body_cap >= requested);
 * 3) SetPart / SetPartByRef bounds checks reject u32 wraparound;
 * 4) Keys >= 1024 bytes are rejected without truncating into shorter keys;
 * 5) SetValue rejects values with missing or truncated attr regions;
 * 6) ViewResolvePrefix stops cleanly at prefix_len==0 nodes (/vthread/1/ beside /vthread/10/);
 * 7) ViewWrite accepts only equal-length writes without overwriting attr;
 * 8) ListLen/ListAt cache invalidates immediately on key-set changes (including cross-process);
 * 9) DSN scheme must be exact "shm";
 * 10) DelTree deletes full subtrees (including U+2025 keys and trailing '/' or U+00B7)
 *     without following pointer targets. */
#define _GNU_SOURCE
#include <kvspace/kvspace.h>
#include "test_util.h"
#include "xvalue_head.h"
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static char g_dir[] = "/tmp/kvspace-abi-contract-XXXXXX";

static void *open_db(const char *name) {
    char dsn[256];
    snprintf(dsn, sizeof dsn, "shm://%s/%s", g_dir, name);
    return kvspaceConnect(dsn);
}

static int set_i64(void *h, const char *key, int64_t v) {
    uint8_t *val = NULL;
    uint32_t n = 0;
    if (kvspaceNewInt64(v, "int64", &val, &n) != 0)
        return -1;
    int rc = kvspaceSetValue(h, key, val, n, NULL, 0);
    free(val);
    return rc;
}

static int set_chars(void *h, const char *key, const char *s) {
    uint8_t *val = NULL;
    uint32_t n = 0;
    if (kvspaceNewChar((const uint8_t *)s, (uint32_t)strlen(s), "char/utf8",
                       &val, &n) != 0)
        return -1;
    int rc = kvspaceSetValue(h, key, val, n, NULL, 0);
    free(val);
    return rc;
}

static uint8_t ptr_byte(void *h, const char *key) {
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, key, 0, &out, &n);
    return out && n >= KVSPACE_XVALUE_HEAD_LEN ? out[7] : 0xFF;
}

static void test_writenewplace_ref(void) {
    void *h = open_db("ref");
    REQUIRE(h, "connect");
    REQUIRE(set_i64(h, "/t", 7) == 0, "set /t");
    uint8_t *body = NULL;
    REQUIRE(kvspaceWriteNewPlace(h, "/p", KVSPACE_REF_PTR, KVSPACE_STORETYPE_SLACK,
                                 2, 2, "int64", &body, NULL, 0) == 0 && body,
            "WriteNewPlace ref=PTR");
    memcpy(body, "/t", 2);
    CHECK(ptr_byte(h, "/p") == KVSPACE_XVALUE_PTR_PTR, "ref=PTR -> ptr byte %u",
          ptr_byte(h, "/p"));
    /* Link is dereferenced when resolve=1. */
    uint8_t *lnk = NULL, *tgt = NULL;
    uint32_t ln = 0, tn = 0;
    kvspaceGet(h, "/p", 1, &lnk, &ln);
    kvspaceGet(h, "/t", 0, &tgt, &tn);
    CHECK(lnk && tgt && ln == tn && memcmp(lnk, tgt, tn) == 0,
          "resolve=1 follows the WriteNewPlace link");

    REQUIRE(kvspaceWriteNewPlace(h, "/e", KVSPACE_REF_EXT, KVSPACE_STORETYPE_EXT, 5,
                                 5, "", &body, NULL, 0) == 0 && body,
            "WriteNewPlace ref=EXT");
    memcpy(body, "s3://", 5);
    CHECK(ptr_byte(h, "/e") == KVSPACE_XVALUE_PTR_EXT, "ref=EXT -> ptr byte %u",
          ptr_byte(h, "/e"));
    CHECK(ptr_byte(h, "/t") == KVSPACE_XVALUE_PTR_INLINE, "inline stays inline");
    /* Invalid (ref, storetype) combination is still rejected. */
    CHECK(kvspaceWriteNewPlace(h, "/bad", KVSPACE_REF_PTR,
                               KVSPACE_STORETYPE_FIXED_SMALL, 8, 8, "", &body,
                               NULL, 0) != 0,
          "ref=PTR + FIXED_SMALL rejected");
    kvspaceClose(h);
}

static void test_writenewplace_body_cap(void) {
    void *h = open_db("cap");
    REQUIRE(h, "connect");
    uint8_t *body = NULL;
    REQUIRE(kvspaceWriteNewPlace(h, "/pc", KVSPACE_REF_INLINE,
                                 KVSPACE_STORETYPE_SLACK, 10, 1024, "", &body,
                                 NULL, 0) == 0 && body,
            "WriteNewPlace cap=1024");
    memcpy(body, "0123456789", 10);
    kvspaceRef_t ref;
    REQUIRE(kvspaceResolveRef(h, "/pc", &ref) == 0, "resolve /pc");
    kvspaceXvalueBodyView_t v;
    REQUIRE(kvspaceXvalueBodyView(h, &ref, "/pc", &v) == 0, "body view");
    CHECK(v.body_cap >= 1024, "view.body_cap=%llu, want >= 1024",
          (unsigned long long)v.body_cap);
    /* Logical body length remains 10 bytes on readback. */
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, "/pc", 0, &out, &n);
    CHECK(out && n == 8 + 10 + 2 && memcmp(out + 8, "0123456789", 10) == 0,
          "value reads back (len=%u)", n);
    /* Rewriting within reserved capacity reuses the existing box in place. */
    uint8_t *body2 = NULL;
    REQUIRE(kvspaceWriteNewPlace(h, "/pc", KVSPACE_REF_INLINE,
                                 KVSPACE_STORETYPE_SLACK, 20, 1024, "", &body2,
                                 NULL, 0) == 0 && body2,
            "WriteNewPlace again");
    CHECK(body2 == body, "box reused when reserve already fits");
    /* body_cap < body_len is still rejected. */
    CHECK(kvspaceWriteNewPlace(h, "/x", KVSPACE_REF_INLINE, KVSPACE_STORETYPE_SLACK,
                               10, 5, "", &body, NULL, 0) != 0,
          "body_cap < body_len rejected");
    kvspaceClose(h);
}

static void test_setpart_overflow(void) {
    void *h = open_db("part");
    REQUIRE(h, "connect");
    REQUIRE(set_i64(h, "/v", 1) == 0, "set /v");
    uint8_t buf[32];
    memset(buf, 0xAB, sizeof buf);
    /* 0xFFFFFFF0 + 32 wraps to 16 in u32; must be rejected without OOB write. */
    CHECK(kvspaceSetPart(h, "/v", 0xFFFFFFF0u, buf, 32, NULL, 0) != 0,
          "SetPart wrapped range rejected");
    kvspaceRef_t ref;
    REQUIRE(kvspaceResolveRef(h, "/v", &ref) == 0, "resolve /v");
    CHECK(kvspaceSetPartByRef(h, &ref, "/v", 0xFFFFFFF0u, buf, 32, NULL, 0) != 0,
          "SetPartByRef wrapped range rejected");
    /* Valid range succeeds. */
    uint8_t nine[8] = {9, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceSetPart(h, "/v", 8, nine, 8, NULL, 0) == 0, "SetPart in range");
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, "/v", 0, &out, &n);
    CHECK(out && out[8] == 9, "in-range SetPart landed");
    kvspaceClose(h);
}

static void test_long_key(void) {
    void *h = open_db("long");
    REQUIRE(h, "connect");
    char k[1024], longk[1100];
    k[0] = '/';
    memset(k + 1, 'a', 1022);
    k[1023] = 0; /* 1023 bytes: longest key accepted by Set */
    snprintf(longk, sizeof longk, "%s/other", k);
    REQUIRE(set_i64(h, k, 7) == 0, "set 1023-byte key");
    CHECK(set_i64(h, longk, 8) != 0, "Set rejects >= 1024-byte key");
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, longk, 0, &out, &n);
    CHECK(!out && n == 0, "Get(long key) is a miss, not its 1023-byte prefix");
    kvspaceGet(h, longk, 1, &out, &n);
    CHECK(!out && n == 0, "Get(long key, resolve) is a miss");
    const char *ks[1] = {longk};
    kvspaceDel(h, ks, 1, NULL, 0);
    kvspaceGet(h, k, 0, &out, &n);
    CHECK(out != NULL, "Del(long key) leaves the 1023-byte key alone");
    uint8_t *body = NULL;
    CHECK(kvspaceWriteInPlace(h, longk, 0, 8, &body, NULL, 0) != 0,
          "WriteInPlace(long key) rejected");
    char longdir[1200];
    snprintf(longdir, sizeof longdir, "%s/", longk);
    int32_t cnt = -1;
    CHECK(kvspaceListLen(h, longdir, 0, 1, &cnt) != 0 || cnt == 0,
          "ListLen(long prefix, resolve) does not list a truncated dir");
    kvspaceClose(h);
}

static void test_setvalue_attr(void) {
    void *h = open_db("attr");
    REQUIRE(h, "connect");
    /* 8B head + 8B body without attr region must be rejected. */
    uint8_t noattr[16] = {8, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceSetValue(h, "/a", noattr, sizeof noattr, NULL, 0) != 0,
          "value without attr region rejected");
    /* attr_len points past end of value. */
    uint8_t badattr[18] = {8, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 50, 0};
    CHECK(kvspaceSetValue(h, "/b", badattr, sizeof badattr, NULL, 0) != 0,
          "attr_len past end rejected");
    /* attr_len < 2. */
    uint8_t shortattr[18] = {8, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 1, 0};
    CHECK(kvspaceSetValue(h, "/c", shortattr, sizeof shortattr, NULL, 0) != 0,
          "attr_len < 2 rejected");
    /* Valid value with trailing padding bytes: accepted and trimmed to 8+len+attr. */
    uint8_t padded[20] = {8, 0, 0, 0, 0, 0, 0, 0, 42, 0, 0, 0, 0, 0, 0, 0, 2, 0,
                          0xEE, 0xEE};
    CHECK(kvspaceSetValue(h, "/d", padded, sizeof padded, NULL, 0) == 0,
          "valid value with trailing bytes accepted");
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, "/d", 0, &out, &n);
    CHECK(out && n == 18, "trailing bytes not part of the value (len=%u)", n);
    kvspaceClose(h);
}

static void test_resolve_prefix(void) {
    void *h = open_db("prefix");
    REQUIRE(h, "connect");
    REQUIRE(set_i64(h, "/vthread/1/x", 11) == 0 && set_i64(h, "/vthread/1/y", 12) == 0 &&
                set_i64(h, "/vthread/10/x", 13) == 0,
            "seed keys");
    REQUIRE(kvspaceViewQuiesce(h, 0) == 0, "unquiesce");
    kvspaceRef_t pref;
    CHECK(kvspaceViewResolvePrefix(h, "/vthread/1/", &pref) == 0,
          "ResolvePrefix(/vthread/1/) with sibling /vthread/10/");
    kvspaceViewToken_t tok;
    if (kvspaceViewResolveFrom(h, &pref, "y", &tok) == 0)
        CHECK(tok.value_len == 8 && tok.body[0] == 12, "prefix ref resolves /vthread/1/y");
    else
        CHECK(0, "ViewResolveFrom(prefix, y)");
    CHECK(kvspaceViewResolvePrefix(h, "/vthread/10/", &pref) == 0,
          "ResolvePrefix(/vthread/10/)");
    CHECK(kvspaceViewResolvePrefix(h, "/vthread/2/", &pref) != 0,
          "ResolvePrefix of a missing dir fails");
    kvspaceClose(h);
}

static void test_viewwrite_len(void) {
    void *h = open_db("view");
    REQUIRE(h, "connect");
    REQUIRE(set_chars(h, "/s", "ab") == 0, "set /s");
    REQUIRE(kvspaceViewQuiesce(h, 0) == 0, "unquiesce");
    kvspaceViewToken_t tok;
    REQUIRE(kvspaceViewResolve(h, "/s", &tok) == 0, "view resolve");
    CHECK(kvspaceViewWrite(h, &tok, (const uint8_t *)"abcdefgh", 8) == 1,
          "longer ViewWrite falls back");
    kvspaceHead_t hd;
    CHECK(kvspaceGetHead(h, "/s", &hd) == 0 && hd.body_len == 2 &&
              strcmp(hd.langtype, "char/utf8") == 0,
          "value and langtype intact after rejected ViewWrite");
    CHECK(kvspaceViewWrite(h, &tok, (const uint8_t *)"xy", 2) == 0,
          "same-length ViewWrite");
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, "/s", 0, &out, &n);
    CHECK(out && memcmp(out + 8, "xy", 2) == 0, "same-length ViewWrite landed");
    CHECK(kvspaceGetHead(h, "/s", &hd) == 0 && strcmp(hd.langtype, "char/utf8") == 0,
          "langtype intact after same-length ViewWrite");
    kvspaceClose(h);
}

/* With per-handle ListLen + ListAt caching, any key appearance/disappearance
 * must be immediately visible (including across processes). */
static int list_names(void *h, const char *dir, char out[][32], int cap) {
    int32_t n = -1;
    if (kvspaceListLen(h, dir, 0, 0, &n) != 0)
        return -1;
    for (int32_t i = 0; i < n && i < cap; i++) {
        uint32_t ol = 0;
        if (kvspaceListAt(h, dir, 0, 0, i, (uint8_t *)out[i], 31, &ol) != 0)
            return -2;
        out[i][ol] = 0;
    }
    return n;
}

static void test_list_cache(void) {
    void *h = open_db("list");
    REQUIRE(h, "connect");
    REQUIRE(set_i64(h, "/d/a", 1) == 0 && set_i64(h, "/d/b", 2) == 0, "seed");
    char nm[8][32];
    CHECK(list_names(h, "/d/", nm, 8) == 2 && !strcmp(nm[0], "a") && !strcmp(nm[1], "b"),
          "initial list a,b");
    REQUIRE(set_i64(h, "/d/c", 3) == 0, "add c");
    CHECK(list_names(h, "/d/", nm, 8) == 3 && !strcmp(nm[2], "c"), "insert visible");
    const char *ka[1] = {"/d/a"};
    kvspaceDel(h, ka, 1, NULL, 0);
    CHECK(list_names(h, "/d/", nm, 8) == 2 && !strcmp(nm[0], "b"), "delete visible");
    REQUIRE(set_i64(h, "/d/a", 4) == 0, "revive a");
    CHECK(list_names(h, "/d/", nm, 8) == 3 && !strcmp(nm[0], "a"), "tombstone revive visible");
    /* Same-key overwrite leaves key set and listing unchanged. */
    REQUIRE(set_i64(h, "/d/a", 5) == 0, "overwrite a");
    CHECK(list_names(h, "/d/", nm, 8) == 3, "overwrite keeps list");
    /* View delete (tombstone) must also invalidate the cache. */
    REQUIRE(kvspaceViewQuiesce(h, 0) == 0, "unquiesce");
    kvspaceViewToken_t tok;
    REQUIRE(kvspaceViewResolve(h, "/d/c", &tok) == 0, "view resolve c");
    CHECK(kvspaceViewDel(h, &tok, "/d/c") == 0, "view del c");
    CHECK(list_names(h, "/d/", nm, 8) == 2, "view delete visible");
    /* Out-of-range index and short buffer fail as before, reporting required length in *out_len. */
    uint8_t b1[1];
    uint32_t ol = 0;
    CHECK(kvspaceListAt(h, "/d/", 0, 0, 9, b1, 1, &ol) != 0, "idx out of range");
    REQUIRE(set_i64(h, "/d/longname", 6) == 0, "add longname");
    int32_t n = 0;
    kvspaceListLen(h, "/d/", 0, 0, &n);
    int found = 0;
    for (int32_t i = 0; i < n; i++) {
        if (kvspaceListAt(h, "/d/", 0, 0, i, b1, 1, &ol) != 0 && ol == 8)
            found = 1;
    }
    CHECK(found, "short buffer reports needed length");
    /* Key-set mutation by another process invalidates this handle's cache. */
    CHECK(list_names(h, "/d/", nm, 8) == 3, "list before child (a,b,longname)");
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
        void *c = open_db("list");
        if (!c)
            _exit(2);
        int rc = set_i64(c, "/d/z", 9);
        const char *kb[1] = {"/d/b"};
        kvspaceDel(c, kb, 1, NULL, 0);
        kvspaceClose(c);
        _exit(rc == 0 ? 0 : 3);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "child wrote");
    int cnt = list_names(h, "/d/", nm, 8);
    int has_z = 0, has_b = 0;
    for (int i = 0; i < cnt; i++) {
        has_z |= !strcmp(nm[i], "z");
        has_b |= !strcmp(nm[i], "b");
    }
    CHECK(cnt == 3 && has_z && !has_b, "cross-process insert/delete visible (n=%d)", cnt);
    kvspaceClose(h);
}

/* DelTree deletes base, base/..., base<U+00B7>..., and base<U+2025>... (including
 * hidden U+2025 keys), normalizes trailing '/' or U+00B7 on prefix, preserves
 * sibling prefixes (/dx, /d2), and invalidates the List cache. */
static int exists(void *h, const char *key) {
    uint8_t *out = NULL;
    uint32_t n = 0;
    kvspaceGet(h, key, 0, &out, &n);
    return out != NULL;
}

static const char *const dt_in[] = {
    "/d", "/d/a", "/d/\xE2\x80\xA5status", "/d/sub/x", "/d/sub/\xE2\x80\xA5pc",
    "/d\xC2\xB7k", "/d\xC2\xB7k/y", "/d\xE2\x80\xA5x",
};
static const char *const dt_out[] = {"/dx", "/d2/a", "/e/\xE2\x80\xA5status"};

static int dt_seed(void *h) {
    if (kvspaceMkindex(h, "/d/", 0, NULL, 0) != 0)
        return -1;
    for (size_t i = 0; i < sizeof dt_in / sizeof *dt_in; i++)
        if (set_i64(h, dt_in[i], (int64_t)i) != 0)
            return -1;
    for (size_t i = 0; i < sizeof dt_out / sizeof *dt_out; i++)
        if (set_i64(h, dt_out[i], 100 + (int64_t)i) != 0)
            return -1;
    return 0;
}

static void dt_check(void *h, const char *how) {
    CHECK(!exists(h, "/d/"), "%s: /d/ index deleted", how);
    for (size_t i = 0; i < sizeof dt_in / sizeof *dt_in; i++)
        CHECK(!exists(h, dt_in[i]), "%s: %s deleted", how, dt_in[i]);
    for (size_t i = 0; i < sizeof dt_out / sizeof *dt_out; i++)
        CHECK(exists(h, dt_out[i]), "%s: %s kept", how, dt_out[i]);
}

static void test_deltree(void) {
    void *h = open_db("deltree");
    void *h2 = open_db("deltree");
    REQUIRE(h && h2, "connect");
    const char *forms[] = {"/d", "/d/", "/d\xC2\xB7"};
    for (size_t f = 0; f < sizeof forms / sizeof *forms; f++) {
        REQUIRE(dt_seed(h) == 0, "seed for DelTree(%s)", forms[f]);
        CHECK(exists(h, "/d/\xE2\x80\xA5status"), "seeded runtime key readable");
        /* Warm List caches for /d/ and /d/sub/ across both handles. */
        int32_t n1 = -1, n2 = -1, n3 = -1;
        kvspaceListLen(h, "/d/", 0, 0, &n1);
        kvspaceListLen(h2, "/d/", 0, 0, &n2);
        kvspaceListLen(h, "/d/sub/", 0, 0, &n3);
        CHECK(n1 == 2 && n2 == 2 && n3 == 1, "list before (%d,%d,%d)", n1, n2, n3);
        CHECK(kvspaceDelTree(h, forms[f], NULL, 0) == 0, "DelTree(%s)", forms[f]);
        dt_check(h, forms[f]);
        kvspaceListLen(h, "/d/", 0, 0, &n1);
        kvspaceListLen(h2, "/d/", 0, 0, &n2);
        kvspaceListLen(h, "/d/sub/", 0, 0, &n3);
        CHECK(n1 == 0 && n2 == 0 && n3 == 0, "list cache invalidated (%d,%d,%d)", n1,
              n2, n3);
        uint32_t ol = 0;
        uint8_t b[8];
        CHECK(kvspaceListAt(h2, "/d/", 0, 0, 0, b, sizeof b, &ol) != 0,
              "ListAt after DelTree misses");
    }
    /* Pointer: delete only the pointer itself, leaving target intact (POSIX rm). */
    REQUIRE(set_i64(h, "/t/a", 1) == 0 && set_i64(h, "/t/\xE2\x80\xA5pc", 2) == 0,
            "seed /t");
    uint8_t *lv = NULL;
    uint32_t ln = 0;
    /* REQUIRE evaluates its condition twice; keep allocating call outside to avoid leaking. */
    int prc = kvspaceNewPtr("", "/t", &lv, &ln);
    REQUIRE(prc == 0, "NewPtr");
    int sp = kvspaceSetValue(h, "/p", lv, ln, NULL, 0);
    int sq = kvspaceSetValue(h, "/q", lv, ln, NULL, 0);
    free(lv);
    REQUIRE(sp == 0 && sq == 0, "set links /p /q");
    CHECK(kvspaceDelTree(h, "/p", NULL, 0) == 0 && !exists(h, "/p"), "DelTree(link)");
    CHECK(kvspaceDelTree(h, "/q/", NULL, 0) == 0 && !exists(h, "/q"), "DelTree(link/)");
    CHECK(exists(h, "/t/a") && exists(h, "/t/\xE2\x80\xA5pc"), "link target intact");
    /* Root: Clear removes everything (including U+2025 keys). */
    CHECK(kvspaceClear(h, NULL, 0) == 0, "Clear");
    CHECK(!exists(h, "/t/a") && !exists(h, "/t/\xE2\x80\xA5pc") && !exists(h, "/dx") &&
              !exists(h, "/e/\xE2\x80\xA5status"),
          "Clear removed everything");
    kvspaceClose(h2);
    kvspaceClose(h);
}

static void test_dsn_scheme(void) {
    char dsn[256];
    snprintf(dsn, sizeof dsn, "s://%s/x", g_dir);
    CHECK(kvspaceConnect(dsn) == NULL, "s:// rejected");
    snprintf(dsn, sizeof dsn, "://%s/x", g_dir);
    CHECK(kvspaceConnect(dsn) == NULL, ":// rejected");
    CHECK(kvspaceConnect(NULL) == NULL, "NULL dsn rejected");
    CHECK(kvspaceConnect("shm://") == NULL, "empty path rejected");
}

static void test_abi_err_reporting(void) {
    void *h = open_db("err");
    REQUIRE(h, "connect");
    char err[128];

    /* kvspaceDel: invalid args return non-zero + non-empty err; missing key stays 0 */
    const char *ks[1] = {"/missing"};
    memset(err, 0, sizeof err);
    CHECK(kvspaceDel(NULL, ks, 1, err, sizeof err) != 0 && err[0] != '\0',
          "Del(NULL h) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceDel(h, NULL, 1, err, sizeof err) != 0 && err[0] != '\0',
          "Del(NULL keys, n>0) fails with err='%s'", err);
    const char *null_key[1] = {NULL};
    memset(err, 0, sizeof err);
    CHECK(kvspaceDel(h, null_key, 1, err, sizeof err) != 0 && err[0] != '\0',
          "Del(NULL key[0]) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceDel(h, ks, 1, err, sizeof err) == 0 && err[0] == '\0',
          "Del(missing key) is idempotent 0");

    /* kvspaceDelTree / kvspaceCp / kvspaceCpTree / kvspaceCpList / kvspaceMkindex /
     * kvspaceRmindexExt / kvspaceClear populate err on failure */
    memset(err, 0, sizeof err);
    CHECK(kvspaceDelTree(h, "/.kvspace-meta", err, sizeof err) != 0 && err[0] != '\0',
          "DelTree(reserved) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceCp(h, "/no-such-src", "/dst", err, sizeof err) != 0 && err[0] != '\0',
          "Cp(missing src) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceCpTree(h, "/a", "/a/sub", err, sizeof err) != 0 && err[0] != '\0',
          "CpTree(into child) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceCpList(h, "/.kvspace-meta", "/dst", err, sizeof err) != 0 && err[0] != '\0',
          "CpList(reserved) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceMkindex(NULL, "/idx/", 0, err, sizeof err) != 0 && err[0] != '\0',
          "Mkindex(NULL h) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceRmindexExt(h, "/no-such-ext/", err, sizeof err) != 0 && err[0] != '\0',
          "RmindexExt(missing) fails with err='%s'", err);
    memset(err, 0, sizeof err);
    CHECK(kvspaceClear(NULL, err, sizeof err) != 0 && err[0] != '\0',
          "Clear(NULL h) fails with err='%s'", err);

    /* Watch(target=None) on a missing/deleted key must wake immediately without waiting out tick_ns. */
    REQUIRE(set_chars(h, "/watch/del", "pc0") == 0, "seed /watch/del");
    const char *del_k[1] = {"/watch/del"};
    REQUIRE(kvspaceDel(h, del_k, 1, err, sizeof err) == 0, "del /watch/del");
    struct timespec wt0, wt1;
    clock_gettime(CLOCK_MONOTONIC, &wt0);
    uint8_t *wout = (uint8_t *)1;
    uint32_t wlen = 99;
    CHECK(kvspaceWatch(h, "/watch/del", (const uint8_t *)"", 0, 5000000000ULL,
                       &wout, &wlen) == 0 &&
              wout == NULL && wlen == 0,
          "Watch(None) wakes immediately on deleted key");
    clock_gettime(CLOCK_MONOTONIC, &wt1);
    uint64_t welapsed_ms =
        (uint64_t)(wt1.tv_sec - wt0.tv_sec) * 1000ULL +
        (uint64_t)(wt1.tv_nsec - wt0.tv_nsec) / 1000000ULL;
    CHECK(welapsed_ms < 500ULL, "Watch(None) elapsed %llums < 500ms",
          (unsigned long long)welapsed_ms);

    kvspaceClose(h);
}

int main(void) {
    if (!mkdtemp(g_dir)) {
        perror("mkdtemp");
        return 1;
    }
    test_writenewplace_ref();
    test_writenewplace_body_cap();
    test_setpart_overflow();
    test_long_key();
    test_setvalue_attr();
    test_resolve_prefix();
    test_viewwrite_len();
    test_list_cache();
    test_deltree();
    test_dsn_scheme();
    test_abi_err_reporting();
    char cmd[128];
    snprintf(cmd, sizeof cmd, "rm -rf %s", g_dir);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed: %s\n", g_dir);
    if (failures) {
        fprintf(stderr, "test_abi_contract: %d failure(s)\n", failures);
        return 1;
    }
    printf("test_abi_contract: ok\n");
    return 0;
}
