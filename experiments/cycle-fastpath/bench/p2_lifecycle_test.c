/* p2_lifecycle_test.c — 单线程叶生命周期：set → resolve/pin → del → sweep → release → re-set → re-resolve */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kvspace_shm.h"
extern int kvspaceNewChar(const uint8_t *, uint32_t, uint8_t **, uint32_t *);
extern int kvspaceShmViewResolve(kvspace_t *, const char *, kvspaceViewToken_t *);
extern int kvspaceShmViewCheck(kvspace_t *, const kvspaceViewToken_t *);
extern int kvspaceShmViewDel(kvspace_t *, kvspaceViewToken_t *, const char *);
extern int kvspaceShmViewSweep(kvspace_t *);

int main(int argc, char **argv) {
    const char *key = argc > 1 ? argv[1] : "/vthread/1/[1]/a";
    const char *store = argc > 2 ? argv[2] : "/tmp/cycle-fastpath/p2/shm/lc.dat";
    uint8_t raw[8] = {'0','1','2','3','4','5','6','7'};
    uint8_t *val = NULL; uint32_t vl = 0;
    if (kvspaceNewChar(raw, 8, &val, &vl) != 0) return 2;
    kvspace_t *kv = kvspaceShmOpen(store, 8ull*64*64*64*64);
    if (!kv) { fprintf(stderr, "open fail\n"); return 2; }

    printf("1 set: %d\n", kvspaceShmSetValue(kv, key, val, (int32_t)vl, 0, 0));
    kvspaceViewToken_t t = {0};
    int rc = kvspaceShmViewResolve(kv, key, &t);
    printf("2 resolve: rc=%d gen=%u vg=%u node=%u\n", rc, t.struct_gen, t.value_gen, t.node_id);
    if (rc != 0) return 1;

    rc = kvspaceShmViewDel(kv, &t, key);
    printf("3 del(tombstone): rc=%d\n", rc);
    int32_t len = 0;
    printf("4 get after del (expect miss): %s\n",
           kvspaceShmGet(kv, key, 1, &len) ? "HIT(bug)" : "miss(ok)");

    int freed = kvspaceShmViewSweep(kv);
    printf("5 sweep: freed=%d\n", freed);
    printf("6 release pin: (removed; no pins)\n");

    printf("7 re-set: %d\n", kvspaceShmSetValue(kv, key, val, (int32_t)vl, 0, 0));
    printf("8 get after re-set: %s\n",
           kvspaceShmGet(kv, key, 1, &len) ? "HIT(ok)" : "miss(bug)");
    kvspaceViewToken_t t2 = {0};
    rc = kvspaceShmViewResolve(kv, key, &t2);
    printf("9 re-resolve: rc=%d gen=%u vg=%u node=%u (old gen=%u)\n",
           rc, t2.struct_gen, t2.value_gen, t2.node_id, t.struct_gen);
    if (rc == 0) printf("10 check: %d  body[0]=%d\n", kvspaceShmViewCheck(kv, &t2), t2.body[0]);
    kvspaceShmClose(kv);
    return rc == 0 ? 0 : 1;
}
