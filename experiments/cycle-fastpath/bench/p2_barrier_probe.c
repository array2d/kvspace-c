/* p2_barrier_probe.c — 静默屏障模型可行性测量（只测量，不改存储语义）。
 *   boxsizes <store>                 各值尺寸的 box 物理容量（tok.cap/body_cap）
 *   ops <store> <n>                  同尺寸覆写 vs 换尺寸覆写的 ns/op
 *   barrier <store> <helpers> <reps> 屏障时延：kill 一个 owner → sweep 到全部 ack 的耗时
 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include "kvspace_shm.h"

extern int kvspaceNewChar(const uint8_t *, uint32_t, uint8_t **, uint32_t *);
extern int kvspaceShmViewResolve(kvspace_t *, const char *, kvspaceViewToken_t *);
extern int kvspaceShmViewCheck(kvspace_t *, const kvspaceViewToken_t *);
extern int kvspaceShmViewRelease(kvspace_t *, kvspaceViewToken_t *);
extern int kvspaceShmViewSweep(kvspace_t *);
extern int kvspaceShmViewQuiesce(kvspace_t *, int);
extern int kvspaceShmViewAck(kvspace_t *, uint32_t *);

#define STORE_SZ (8ull * 64 * 64 * 64 * 64)
static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + ts.tv_nsec / 1e9;
}
static uint8_t *mk(uint32_t n, uint32_t *out_len) {
    uint8_t *raw = calloc(n ? n : 1, 1);
    for (uint32_t i = 0; i < n; i++) raw[i] = (uint8_t)('a' + i % 26);
    uint8_t *v = NULL; uint32_t vl = 0;
    if (kvspaceNewChar(raw, n, &v, &vl) != 0) { free(raw); return NULL; }
    free(raw);
    if (out_len) *out_len = vl;
    return v;
}

static int m_boxsizes(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) return 2;
    if (kvspaceShmViewQuiesce(kv, 0) != 0) return 2;   /* 用 view 前必须退出默认静默 */
    uint32_t sizes[] = {1, 8, 16, 24, 32, 48, 64, 128, 512};
    printf("value_bytes,wire_len,box_cap,body_cap\n");
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        uint32_t vl = 0;
        uint8_t *v = mk(sizes[i], &vl);
        if (!v) return 2;
        char key[64];
        snprintf(key, sizeof key, "/vthread/1/[1]/s%u", sizes[i]);
        if (kvspaceShmSetValue(kv, key, v, (int32_t)vl, 0, 0) != 0) return 2;
        kvspaceViewToken_t t = {0};
        if (kvspaceShmViewResolve(kv, key, &t) != 0) return 2;
        printf("%u,%u,%llu,%llu\n", sizes[i], vl,
               (unsigned long long)t.cap, (unsigned long long)t.body_cap);
        kvspaceShmViewRelease(kv, &t);
        free(v);
    }
    kvspaceShmClose(kv);
    return 0;
}

static int m_ops(const char *store, uint64_t n) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) return 2;
    uint32_t l8 = 0, l64 = 0;
    uint8_t *v8 = mk(8, &l8), *v64 = mk(64, &l64);
    const char *key = "/vthread/1/[2]/o";
    kvspaceShmSetValue(kv, key, v8, (int32_t)l8, 0, 0);
    double t0 = now_s();
    for (uint64_t i = 0; i < n; i++) kvspaceShmSetValue(kv, key, v8, (int32_t)l8, 0, 0);
    double t1 = now_s();
    /* 换尺寸：8 ↔ 64 交替，每次旧 box 成为垃圾 */
    for (uint64_t i = 0; i < n; i++)
        kvspaceShmSetValue(kv, key, (i & 1) ? v64 : v8,
                           (i & 1) ? (int32_t)l64 : (int32_t)l8, 0, 0);
    double t2 = now_s();
    /* 同尺寸但 64B 值：衡量 resize 后稳态 */
    kvspaceShmSetValue(kv, key, v64, (int32_t)l64, 0, 0);
    double t3 = now_s();
    for (uint64_t i = 0; i < n; i++) kvspaceShmSetValue(kv, key, v64, (int32_t)l64, 0, 0);
    double t4 = now_s();
    printf("same_8B_ns_per_op=%.1f resize_8_64_ns_per_op=%.1f same_64B_ns_per_op=%.1f\n",
           (t1 - t0) / n * 1e9, (t2 - t1) / n * 1e9, (t4 - t3) / n * 1e9);
    kvspaceShmClose(kv);
    return 0;
}

/* 每次操作之间插入的延迟，模拟真实进程的操作节奏（acк 只发生在操作边界）。 */
static int helper_delay_us(void) {
    const char *e = getenv("P2_HELPER_DELAY_US");
    return e ? atoi(e) : 0;
}

static int m_barrier(const char *store, int helpers, int reps) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) return 2;
    pid_t *hp = calloc((size_t)helpers, sizeof *hp);
    for (int i = 0; i < helpers; i++) {
        pid_t p = fork();
        if (p == 0) {
            kvspace_t *k2 = kvspaceShmOpen(store, STORE_SZ);
            if (!k2) _exit(2);
            if (kvspaceShmViewQuiesce(k2, 0) != 0) _exit(2);
            uint32_t vl = 0;
            uint8_t *v = mk(8, &vl);
            char key[64];
            snprintf(key, sizeof key, "/vthread/1/[%d]/b", 10 + i);
            kvspaceShmSetValue(k2, key, v, (int32_t)vl, 0, 0);
            kvspaceViewToken_t t = {0};
            if (kvspaceShmViewResolve(k2, key, &t) != 0) _exit(2);
            int delay = helper_delay_us();
            uint32_t ep = 0;
            for (;;) {
                (void)kvspaceShmViewAck(k2, &ep);         /* Q1：op 边界 ack */
                (void)kvspaceShmViewCheck(k2, &t);
                if (delay > 0) usleep((useconds_t)delay);
            }
        }
        hp[i] = p;
    }
    usleep(200000);
    double sum = 0, worst = 0;
    for (int r = 0; r < reps; r++) {
        pid_t victim = fork();
        if (victim == 0) {
            kvspace_t *k3 = kvspaceShmOpen(store, STORE_SZ);
            if (!k3) _exit(2);
            for (;;) pause();
        }
        usleep(20000);
        kill(victim, SIGKILL);
        waitpid(victim, NULL, 0);
        double t0 = now_s();
        kvspaceShmViewSweep(kv);               /* 屏障：推进 epoch → 等存活槽 ack */
        double dt = (now_s() - t0) * 1e6;
        sum += dt;
        if (dt > worst) worst = dt;
    }
    for (int i = 0; i < helpers; i++) kill(hp[i], SIGKILL);
    for (int i = 0; i < helpers; i++) waitpid(hp[i], NULL, 0);
    kvspaceShmClose(kv);
    printf("helpers=%d reps=%d barrier_avg_us=%.1f worst_us=%.1f\n",
           helpers, reps, sum / reps, worst);
    return 0;
}

static int m_sweep(const char *store, int n) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) return 2;
    double t0 = now_s();
    for (int i = 0; i < n; i++) kvspaceShmViewSweep(kv);
    double dt = (now_s() - t0) / n * 1e6;
    kvspaceShmClose(kv);
    printf("sweep_us_per_call=%.1f (n=%d)\n", dt, n);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s boxsizes|ops|barrier|sweep <store> [...]\n", argv[0]);
        return 2;
    }
    if (!strcmp(argv[1], "sweep"))
        return m_sweep(argv[2], argc > 3 ? atoi(argv[3]) : 1000);
    if (!strcmp(argv[1], "boxsizes")) return m_boxsizes(argv[2]);
    if (!strcmp(argv[1], "ops")) return m_ops(argv[2], argc > 3 ? strtoull(argv[3], NULL, 10) : 200000);
    if (!strcmp(argv[1], "barrier"))
        return m_barrier(argv[2], argc > 3 ? atoi(argv[3]) : 2,
                         argc > 4 ? atoi(argv[4]) : 50);
    return 2;
}
