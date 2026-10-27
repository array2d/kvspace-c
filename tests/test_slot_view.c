/* P1b4-b1a：shm slot view 语义。
 * 覆盖 cap/body_cap 自洽、status 固定形状与容量、旧小 cap 槽拒绝、
 * content 越界拒绝、10 万次写指针稳定。 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "failed: %s (line %d)\n", #x, __LINE__);           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static uint8_t *slack(const char *s, uint64_t *n) {
    uint8_t *v = NULL;
    if (kvspaceXhNewSlack(KVSPACE_XH_UTF8, (const uint8_t *)s, strlen(s),
                          strlen(s), &v, n) != 0)
        return NULL;
    return v;
}

int main(void) {
    char dir[] = "/tmp/kvspace-slot-view-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/db", dir);
    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    CHECK(kv != NULL);

    /* 普通 SLACK 槽：cap/body_cap 自洽、content 不越界。 */
    uint64_t n = 0;
    uint8_t *v = slack("hello", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, "/a/s", v, (int32_t)n) == 0);
    free(v);
    kvspaceRef_t r;
    memset(&r, 0, sizeof r);
    CHECK(kvspaceShmResolveRef(kv, "/a/s", &r) == 0);
    kvspaceXvalueBodyView_t view;
    CHECK(kvspaceShmXvalueBodyView(kv, &r, "/a/s", &view) == 0);
    CHECK(view.head && view.body == view.head + view.headlen);
    CHECK(view.cap >= view.headlen && view.body_cap == view.cap - view.headlen);
    kvspaceXh xh;
    CHECK(kvspaceXhDecode(view.head, view.cap, &xh) == 0);
    CHECK(xh.content_len <= view.body_cap);

    /* key_fallback 必填：NULL → -1。 */
    kvspaceXvalueBodyView_t nv;
    CHECK(kvspaceShmXvalueBodyView(kv, &r, NULL, &nv) == -1);

    /* status：短值 / 16B 合法，17B 与异型硬错。 */
    const char *sk = "/vthread/7/\xE2\x80\xA5status";
    v = slack("init", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
    free(v);
    memset(&r, 0, sizeof r);
    CHECK(kvspaceShmResolveRef(kv, sk, &r) == 0);
    CHECK(kvspaceShmXvalueBodyView(kv, &r, sk, &view) == 0);
    uint8_t *head0 = view.head, *body0 = view.body;
    uint64_t cap0 = view.cap;
    CHECK(view.headlen == (1u << KVSPACE_XH_POW_SLACK));
    CHECK(view.body_cap >= 16);

    for (int i = 0; i < 100000; i++) {
        v = slack("running", &n);
        CHECK(v != NULL);
        CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
        free(v);
        kvspaceXvalueBodyView_t again;
        CHECK(kvspaceShmXvalueBodyView(kv, &r, sk, &again) == 0);
        CHECK(again.head == head0 && again.body == body0 && again.cap == cap0);
    }
    v = slack("0123456789abcdef", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) == 0);
    free(v);
    v = slack("0123456789abcdefg", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) != 0);
    free(v);
    uint8_t raw[8] = {1};
    CHECK(kvspaceXhNewScalar("int64", raw, 8, &v, &n) == 0);
    CHECK(kvspaceShmSet(kv, sk, v, (int32_t)n) != 0);
    free(v);

    /* 旧小 cap status 槽：view -1，写入硬错。 */
    const char *lk = "/vthread/8/\xE2\x80\xA5status";
    uint8_t *lbody = NULL;
    CHECK(kvspaceShmWriteNewPlace(kv, lk, 0, KVSPACE_XH_SLACK, 0, 0,
                                  "[3]char/utf8", 0, 0, &lbody) == 0);
    kvspaceRef_t lr;
    memset(&lr, 0, sizeof lr);
    CHECK(kvspaceShmResolveRef(kv, lk, &lr) == 0);
    kvspaceXvalueBodyView_t lv;
    CHECK(kvspaceShmXvalueBodyView(kv, &lr, lk, &lv) == -1);
    v = slack("init", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, lk, v, (int32_t)n) != 0);
    free(v);

    /* 旧槽同 headlen 但 langtype 非 [n]char/utf8：view -1、写入硬错。 */
    const char *wk = "/vthread/9/\xE2\x80\xA5status";
    uint8_t *wbody = NULL;
    CHECK(kvspaceShmWriteNewPlace(kv, wk, 0, KVSPACE_XH_SLACK, 0, 0,
                                  "[3]char/ascii", 0, 16, &wbody) == 0);
    kvspaceRef_t wr;
    memset(&wr, 0, sizeof wr);
    CHECK(kvspaceShmResolveRef(kv, wk, &wr) == 0);
    kvspaceXvalueBodyView_t wv;
    CHECK(kvspaceShmXvalueBodyView(kv, &wr, wk, &wv) == -1);
    v = slack("init", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, wk, v, (int32_t)n) != 0);
    free(v);

    /* status content 被篡改成 17：view -1。 */
    kvspaceRef_t s17;
    memset(&s17, 0, sizeof s17);
    CHECK(kvspaceShmResolveRef(kv, sk, &s17) == 0);
    uint8_t a17[8] = {17, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceShmSetPartByRef(kv, &s17, sk, 2, a17, 8) == 0);
    kvspaceXvalueBodyView_t v17;
    CHECK(kvspaceShmXvalueBodyView(kv, &s17, sk, &v17) == -1);
    CHECK(kvspaceShmXvalueBodyView(kv, &s17, NULL, &v17) == -1);

    /* 越界 content（head.a 被改大）：view -1。 */
    v = slack("hello", &n);
    CHECK(v != NULL);
    CHECK(kvspaceShmSet(kv, "/a/corrupt", v, (int32_t)n) == 0);
    free(v);
    uint8_t big[8] = {0x40, 0x42, 0x0f, 0, 0, 0, 0, 0};
    kvspaceRef_t xr;
    memset(&xr, 0, sizeof xr);
    CHECK(kvspaceShmResolveRef(kv, "/a/corrupt", &xr) == 0);
    CHECK(kvspaceShmSetPartByRef(kv, &xr, "/a/corrupt", 2, big, 8) == 0);
    kvspaceRef_t cr;
    memset(&cr, 0, sizeof cr);
    CHECK(kvspaceShmResolveRef(kv, "/a/corrupt", &cr) == 0);
    kvspaceXvalueBodyView_t cv;
    CHECK(kvspaceShmXvalueBodyView(kv, &cr, "/a/corrupt", &cv) == -1);

    /* 不存在的 key → -1。 */
    kvspaceRef_t miss;
    memset(&miss, 0xff, sizeof miss);
    kvspaceXvalueBodyView_t mv;
    CHECK(kvspaceShmXvalueBodyView(kv, &miss, "/no/such", &mv) == -1);

    kvspaceShmClose(kv);
    printf("ok: slot view\n");
    return 0;
}
