/* p2_threads_test.c — 同进程并发：读者 token 循环 / 写者覆写+删除 / sweeper。
 * TSan/ASan 目标。用法：
 *   p2_threads_test <store> <seconds> <readers> [writer_us] [sweep_us]
 * 断言：无崩溃；读者读到的 body[0] 只能是 'A'/'B'；STALE 计数可非零；结束前最后一次写可见。 */
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "kvspace_shm.h"

extern int kvspaceNewChar(const uint8_t *, uint32_t, uint8_t **, uint32_t *);
extern int kvspaceShmViewResolve(kvspace_t *, const char *, kvspaceViewToken_t *);
extern int kvspaceShmViewCheck(kvspace_t *, const kvspaceViewToken_t *);
extern int kvspaceShmViewRelease(kvspace_t *, kvspaceViewToken_t *);
extern int kvspaceShmViewSweep(kvspace_t *);

static const char *g_store;
static atomic_int g_stop;
static atomic_ullong g_ok, g_stale, g_busy, g_del, g_sweep, g_bad;

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}
static void nap_us(int us) { if (us > 0) usleep((useconds_t)us); }

static void *reader(void *arg) {
    long idx = (long)arg;
    char key[64];
    snprintf(key, sizeof key, "/vthread/1/[%ld]/t0", idx);
    kvspace_t *kv = kvspaceShmOpen(g_store, 8ull * 64 * 64 * 64 * 64);
    if (!kv) { atomic_fetch_add(&g_bad, 1); return NULL; }
    while (!atomic_load(&g_stop)) {
        kvspaceViewToken_t t = {0};
        int rc = kvspaceShmViewResolve(kv, key, &t);
        if (rc == 1) { atomic_fetch_add(&g_stale, 1); continue; }
        if (rc != 0) { atomic_fetch_add(&g_busy, 1); continue; }
        int c = kvspaceShmViewCheck(kv, &t);
        if (c == 0) {
            uint8_t b = t.body ? t.body[0] : 0;
            if (b != 'A' && b != 'B') atomic_fetch_add(&g_bad, 1);
            atomic_fetch_add(&g_ok, 1);
        } else {
            atomic_fetch_add(&g_stale, 1);
        }
        kvspaceShmViewRelease(kv, &t);
    }
    kvspaceShmClose(kv);
    return NULL;
}

static void *writer(void *arg) {
    (void)arg;
    uint8_t a[8] = {'A'}, b[8] = {'B'};
    uint8_t *va = NULL, *vb = NULL;
    uint32_t la = 0, lb = 0;
    kvspaceNewChar(a, 8, &va, &la);
    kvspaceNewChar(b, 8, &vb, &lb);
    kvspace_t *kv = kvspaceShmOpen(g_store, 8ull * 64 * 64 * 64 * 64);
    if (!kv) { atomic_fetch_add(&g_bad, 1); return NULL; }
    char key[64];
    snprintf(key, sizeof key, "/vthread/1/[0]/t0");
    unsigned n = 0;
    while (!atomic_load(&g_stop)) {
        const uint8_t *v = (n & 1) ? vb : va;
        uint32_t l = (n & 1) ? lb : la;
        kvspaceShmSetValue(kv, key, v, (int32_t)l, 0, 0);
        n++;
        if ((n % 37) == 0) {           /* 周期性删除，制造墓碑/退役路径 */
            kvspaceShmDel(kv, key);
            atomic_fetch_add(&g_del, 1);
        }
        nap_us(50);
    }
    kvspaceShmClose(kv);
    return NULL;
}

static void *sweeper(void *arg) {
    int us = (int)(intptr_t)arg;
    kvspace_t *kv = kvspaceShmOpen(g_store, 8ull * 64 * 64 * 64 * 64);
    if (!kv) { atomic_fetch_add(&g_bad, 1); return NULL; }
    while (!atomic_load(&g_stop)) {
        kvspaceShmViewSweep(kv);
        atomic_fetch_add(&g_sweep, 1);
        nap_us(us);
    }
    kvspaceShmClose(kv);
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s <store> <seconds> <readers> [writer_us] [sweep_us]\n", argv[0]);
        return 2;
    }
    g_store = argv[1];
    double secs = atof(argv[2]);
    int nr = atoi(argv[3]);
    int wus = argc > 4 ? atoi(argv[4]) : 50;
    int sus = argc > 5 ? atoi(argv[5]) : 1000;
    if (nr < 1 || nr > 32) return 2;

    kvspace_t *kv = kvspaceShmOpen(g_store, 8ull * 64 * 64 * 64 * 64);
    if (!kv) { fprintf(stderr, "open fail\n"); return 2; }
    uint8_t a[8] = {'A'}, b[8] = {'B'};
    uint8_t *va = NULL, *vb = NULL;
    uint32_t la = 0, lb = 0;
    kvspaceNewChar(a, 8, &va, &la);
    kvspaceNewChar(b, 8, &vb, &lb);
    for (int i = 0; i < nr; i++) {
        char key[64];
        snprintf(key, sizeof key, "/vthread/1/[%d]/t0", i);
        kvspaceShmSetValue(kv, key, va, (int32_t)la, 0, 0);
    }
    kvspaceShmClose(kv);

    pthread_t *rt = calloc((size_t)nr, sizeof *rt);
    pthread_t wt, st;
    for (long i = 0; i < nr; i++) pthread_create(&rt[i], NULL, reader, (void *)i);
    pthread_create(&wt, NULL, writer, NULL);
    pthread_create(&st, NULL, sweeper, (void *)(intptr_t)sus);

    double t0 = now_s();
    nap_us(wus);
    while (now_s() - t0 < secs) nap_us(10000);
    atomic_store(&g_stop, 1);
    for (int i = 0; i < nr; i++) pthread_join(rt[i], NULL);
    pthread_join(wt, NULL);
    pthread_join(st, NULL);

    /* 最终可见性：写 'A' 再读回。 */
    kvspace_t *kv2 = kvspaceShmOpen(g_store, 8ull * 64 * 64 * 64 * 64);
    int rc = kvspaceShmSetValue(kv2, "/vthread/1/[0]/t0", va, (int32_t)la, 0, 0);
    int32_t len = 0;
    uint8_t *got = kvspaceShmGet(kv2, "/vthread/1/[0]/t0", 1, &len);
    int final_ok = (rc == 0 && got && len == (int32_t)la &&
                    memcmp(got, va, la) == 0);
    kvspaceShmClose(kv2);

    printf("ok=%llu stale=%llu busy=%llu del=%llu sweep=%llu bad=%llu final=%s\n",
           (unsigned long long)g_ok, (unsigned long long)g_stale,
           (unsigned long long)g_busy, (unsigned long long)g_del,
           (unsigned long long)g_sweep, (unsigned long long)g_bad,
           final_ok ? "ok" : "FAIL");
    int fail = atomic_load(&g_bad) != 0 || !final_ok;
    return fail ? 1 : 0;
}
