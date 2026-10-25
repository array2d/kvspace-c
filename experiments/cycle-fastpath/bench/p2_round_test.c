/* p2_round_test.c — 第 3 步屏障轮 / Q1-Q2 测试。
 *
 * 构建（必须带测试开关，库与测试都要）：
 *   cmake -S p2/src/kvspace-c -B /tmp/p2s3t -DCMAKE_BUILD_TYPE=Release \
 *         -DCMAKE_C_FLAGS=-DP2_TEST_HOOKS
 *   cmake --build /tmp/p2s3t -j
 *   gcc -O2 -DP2_TEST_HOOKS -I p2/src/kvspace-c/src -o /tmp/p2rt \
 *       bench/p2_round_test.c -L/tmp/p2s3t -lkvspace-c -Wl,-rpath,/tmp/p2s3t -pthread
 *
 * 模式（每个用独立全新 store）：
 *   t1   单进程 sweep + garb_bytes 归零（T1/T6）
 *   t4   同进程 leader 不 ack → 超时零 free；解释器线程 ack 后成功（A23）
 *   t3   活 owner 在跑时另一进程 Sweep 立即 -1（不等待）
 *   a19  两进程同删一键：一个 CAS 赢、一个 cancel，只 free 一次
 *   a20  写者卡在 reserve→commit：drain 不得跨过未决条
 *   a13  读者在 Check→读窗口：轮次放弃、零 free；ack 后下一轮回收
 *   a14  同上 + 窗口内 SIGSTOP
 *   a15  默认 quiesced=1 的空进程不阻塞轮次（A22）
 *   a16  Quiesce(0) 后既不 ack 也不声明：每轮放弃；kill 后恢复
 *   a6   leader 在 drain 中 kill -9：下一轮接管，只漏一个 box，不 double free
 *   a25  reserve 后 alloc / art_ins 失败 → cancel，旧值不变、无残留
 *   a27  leader 槽被回收后（旧 claim 消失）仍能接管
 *   churn <store> <ms>  持续制造垃圾的 helper（R4 用），结束时打印轮次统计
 */
#define _GNU_SOURCE
#include <pthread.h>
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
extern int kvspaceShmViewAck(kvspace_t *, uint32_t *);
extern int kvspaceShmViewQuiesce(kvspace_t *, int);
extern int kvspaceShmViewResolve(kvspace_t *, const char *, kvspaceViewToken_t *);
extern int kvspaceShmViewCheck(kvspace_t *, const kvspaceViewToken_t *);
extern int kvspaceShmViewWrite(kvspace_t *, kvspaceViewToken_t *,
                               const uint8_t *, uint32_t);
extern int kvspaceShmViewDel(kvspace_t *, kvspaceViewToken_t *, const char *);
extern int kvspaceShmViewRefresh(kvspace_t *, kvspaceViewToken_t *, const char *);
extern int kvspaceShmViewSweep(kvspace_t *);
extern uint64_t kvspaceShmViewGarbBytes(kvspace_t *);
extern void kvspaceShmViewRoundStats(kvspace_t *, uint64_t *, uint64_t *, uint64_t *);

#define STORE_SZ (8ull * 64 * 64 * 64 * 64)
#define FAIL(...)                                                              \
    do {                                                                       \
        fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);                    \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        exit(1);                                                               \
    } while (0)

static uint8_t *mkval(uint32_t n, uint32_t *out) {
    char *raw = calloc(n, 1);
    memset(raw, 'a', n);
    uint8_t *v = NULL;
    if (kvspaceNewChar((const uint8_t *)raw, n, &v, out) != 0)
        FAIL("newchar");
    free(raw);
    return v;
}

static void put(kvspace_t *kv, const char *k, const uint8_t *v, uint32_t l) {
    if (kvspaceShmSetValue(kv, k, v, (int32_t)l, 0, 0) != 0)
        FAIL("set %s", k);
}

/* 建一条 committed 垃圾：删掉已存在的键（retire 一个 box）。 */
static void churn(kvspace_t *kv, const uint8_t *v, uint32_t l, int n) {
    for (int i = 0; i < n; i++) {
        put(kv, "/g", v, l);
        int drc = kvspaceShmDel(kv, "/g");
        if (drc != 0)
            FAIL("del /g at %d rc=%d", i, drc);
    }
}

static void quiesce_off(kvspace_t *kv) {
    if (kvspaceShmViewQuiesce(kv, 0) != 0)
        FAIL("quiesce off");
}

/* 两步 Q1：返回 0 表示已 ack。 */
static void ack_now(kvspace_t *kv) {
    uint32_t ep = 0;
    for (int i = 0; i < 8; i++) {
        int rc = kvspaceShmViewAck(kv, &ep);
        if (rc == 0)
            return;
        if (rc != 1)
            FAIL("ack rc=%d", rc);
    }
    FAIL("ack did not settle");
}

static int rd(int fd) {
    char c;
    if (read(fd, &c, 1) != 1)
        FAIL("pipe read");
    return c;
}
static void wr(int fd, char c) {
    if (write(fd, &c, 1) != 1)
        FAIL("pipe write");
}

static uint32_t box_cap(kvspace_t *kv) {
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    put(kv, "/cap", v, l);
    quiesce_off(kv);
    kvspaceViewToken_t t = {0};
    if (kvspaceShmViewResolve(kv, "/cap", &t) != 0)
        FAIL("resolve cap");
    uint32_t cap = (uint32_t)t.cap;
    if (kvspaceShmViewQuiesce(kv, 1) != 0)
        FAIL("quiesce on");
    if (kvspaceShmDel(kv, "/cap") != 0)
        FAIL("del cap");
    return cap;                        /* 这条也 retired，但 t1 会先清零再数 */
}

/* T1/T6：单进程（默认 quiesced=1）强制一轮，garb_bytes 归零。 */
static int m_t1(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    int n = 50;
    churn(kv, v, l, n);
    uint32_t cap = box_cap(kv);
    uint64_t before = kvspaceShmViewGarbBytes(kv);
    if (before == 0) FAIL("no garbage");
    int freed = kvspaceShmViewSweep(kv);
    if (freed != n + 1) FAIL("freed=%d want %d", freed, n + 1);
    if (before != (uint64_t)freed * cap) FAIL("bytes=%llu want %d*%u",
        (unsigned long long)before, freed, cap);
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    if (kvspaceShmViewSweep(kv) != 0) FAIL("second sweep must free 0");
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero (2)");
    kvspaceShmClose(kv);
    puts("PASS t1");
    return 0;
}

static volatile int g_ack_run;
static uint32_t g_ack_ep;
static void *ack_loop(void *arg) {
    kvspace_t *kv = arg;
    while (g_ack_run) {
        uint32_t ep = g_ack_ep;
        int rc = kvspaceShmViewAck(kv, &ep);
        g_ack_ep = ep;
        (void)rc;                      /* rc==1：无 token，下一轮直接 ack */
        usleep(500);
    }
    return NULL;
}

/* T4：same-process leader，解释器既不 ack 也不 quiesce → 超时零 free；
 * 解释器线程（helper）在 op 边界 ack 后下一轮成功。 */
static int m_t4(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    quiesce_off(kv);
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    churn(kv, v, l, 10);
    uint64_t before = kvspaceShmViewGarbBytes(kv);
    if (before == 0) FAIL("no garbage");
    int rc = kvspaceShmViewSweep(kv);
    if (rc != -2) FAIL("sweep rc=%d want -2 (abort)", rc);
    if (kvspaceShmViewGarbBytes(kv) != before) FAIL("aborted round freed");
    pthread_t th;
    g_ack_run = 1; g_ack_ep = 0;
    if (pthread_create(&th, NULL, ack_loop, kv) != 0) FAIL("ack thread");
    int freed = kvspaceShmViewSweep(kv);
    g_ack_run = 0;
    pthread_join(th, NULL);
    if (freed != 10) FAIL("freed=%d want 10", freed);
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    kvspaceShmClose(kv);
    puts("PASS t4");
    return 0;
}

/* A19：两个进程同时 ViewDel 同一键；一个 CAS 成功、另一个 cancel。 */
static int m_a19(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    put(kv, "/k", v, l);
    int go[2], done[2];
    if (pipe(go) || pipe(done)) FAIL("pipe");
    setenv("P2_TEST_HOLD_DELCAS_MS", "120", 1);
    for (int i = 0; i < 2; i++) {
        pid_t p = fork();
        if (p == 0) {
            close(go[1]); close(done[0]);
            kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
            if (!c) _exit(2);
            quiesce_off(c);
            kvspaceViewToken_t t = {0};
            if (kvspaceShmViewResolve(c, "/k", &t) != 0) _exit(3);
            rd(go[0]);
            if (!kvspaceShmViewCheck(c, &t)) {
                int rc = kvspaceShmViewDel(c, &t, "/k");
                wr(done[1], (char)(rc == 0 ? 'W' : rc == 1 ? 'L' : 'E'));
            } else {
                wr(done[1], 'S');
            }
            _exit(0);
        }
    }
    close(go[0]); close(done[1]);
    wr(go[1], 'g'); wr(go[1], 'g');   /* 两个子进程同时过 reserve 后的暂停 */
    char r1 = (char)rd(done[0]), r2 = (char)rd(done[0]);
    int wins = (r1 == 'W') + (r2 == 'W'), loses = (r1 == 'L') + (r2 == 'L');
    if (wins != 1 || loses != 1) FAIL("wins=%d loses=%d (%c %c)", wins, loses, r1, r2);
    int st1, st2;
    wait(&st1); wait(&st2);
    int freed = kvspaceShmViewSweep(kv);
    if (freed != 1) FAIL("freed=%d want 1", freed);
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    kvspaceShmClose(kv);
    puts("PASS a19");
    return 0;
}

/* A20：写者卡在 reserve→commit，drain 不得跨过它。 */
static int m_a20(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l8 = 0, l512 = 0;
    uint8_t *v8 = mkval(8, &l8), *v512 = mkval(512, &l512);
    put(kv, "/k", v8, l8);
    int go[2], done[2];
    if (pipe(go) || pipe(done)) FAIL("pipe");
    pid_t p = fork();
    if (p == 0) {
        close(go[1]); close(done[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        setenv("P2_TEST_HOLD_RESERVE_MS", "800", 1);
        rd(go[0]);
        /* 换尺寸写：reserve 后暂停 800ms，再 commit */
        if (kvspaceShmSetValue(c, "/k", v512, (int32_t)l512, 0, 0) != 0)
            _exit(3);
        wr(done[1], 'D');
        _exit(0);
    }
    close(go[0]); close(done[1]);
    wr(go[1], 'g');
    usleep(200 * 1000);                /* 让子进程停在 reserve→commit 之间 */
    churn(kv, v8, l8, 20);             /* 这些条目排在保留条之后 */
    uint64_t before = kvspaceShmViewGarbBytes(kv);
    if (before == 0) FAIL("no garbage");
    int freed = kvspaceShmViewSweep(kv);
    if (freed != 0) FAIL("drain crossed a RESERVED entry: freed=%d", freed);
    if (kvspaceShmViewGarbBytes(kv) != before) FAIL("bytes changed at reserved");
    rd(done[0]);                       /* 子进程 commit 完 */
    wait(NULL);
    freed = kvspaceShmViewSweep(kv);
    if (freed != 21) FAIL("freed=%d want 21", freed);
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    kvspaceShmClose(kv);
    puts("PASS a20");
    return 0;
}

/* A13/A14：读者在 Check→读窗口内；轮次必须放弃且零 free。 */
static int m_a13(const char *store, int use_stop) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    put(kv, "/k", v, l);
    churn(kv, v, l, 10);
    int go[2], ready[2], acked[2];
    if (pipe(go) || pipe(ready) || pipe(acked)) FAIL("pipe");
    pid_t p = fork();
    if (p == 0) {
        close(go[1]); close(ready[0]); close(acked[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        quiesce_off(c);
        kvspaceViewToken_t t = {0};
        if (kvspaceShmViewResolve(c, "/k", &t) != 0) _exit(3);
        rd(go[0]);
        wr(ready[1], 'r');
        usleep(400 * 1000);            /* Check→读窗口 */
        /* 轮次推进 epoch 后旧 token 必 STALE：Refresh 重解析再读（box 未被 free）。 */
        if (kvspaceShmViewCheck(c, &t) != 0 &&
            kvspaceShmViewRefresh(c, &t, "/k") != 0)
            _exit(4);
        if (memcmp(t.body, v + t.headlen, l - t.headlen) != 0) _exit(5);
        ack_now(c);
        wr(acked[1], 'a');
        _exit(0);
    }
    close(go[0]); close(ready[1]); close(acked[1]);
    setenv("P2_BARRIER_MS", "50", 1);
    wr(go[1], 'g');
    rd(ready[0]);
    if (use_stop) {
        kill(p, SIGSTOP);
        int rc = kvspaceShmViewSweep(kv);
        if (rc != -2) FAIL("stopped-reader sweep rc=%d want -2", rc);
        kill(p, SIGCONT);
    } else {
        int rc = kvspaceShmViewSweep(kv);
        if (rc != -2) FAIL("reader-window sweep rc=%d want -2", rc);
    }
    uint64_t g = kvspaceShmViewGarbBytes(kv);
    if (g == 0) FAIL("garbage vanished during read window");
    rd(acked[0]);
    wait(NULL);
    int freed = kvspaceShmViewSweep(kv);
    if (freed != 10) FAIL("freed=%d want 10", freed);
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    kvspaceShmClose(kv);
    printf("PASS %s\n", use_stop ? "a14" : "a13");
    return 0;
}

/* A15/T5：默认 quiesced=1 的空进程不阻塞轮次；之后 Quiesce(0) 可正常用 view。 */
static int m_a15(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    put(kv, "/k", v, l);
    churn(kv, v, l, 5);
    int go[2], ready[2], done[2];
    if (pipe(go) || pipe(ready) || pipe(done)) FAIL("pipe");
    pid_t p = fork();
    if (p == 0) {
        close(go[1]); close(ready[0]); close(done[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        wr(ready[1], 'r');             /* 默认 quiesced=1，直接空转 */
        rd(go[0]);
        quiesce_off(c);
        kvspaceViewToken_t t = {0};
        if (kvspaceShmViewResolve(c, "/k", &t) != 0) _exit(3);
        if (kvspaceShmViewCheck(c, &t) != 0) _exit(4);
        wr(done[1], 'd');
        _exit(0);
    }
    close(go[0]); close(ready[1]); close(done[1]);
    rd(ready[0]);
    int freed = kvspaceShmViewSweep(kv);
    if (freed != 5) FAIL("freed=%d want 5", freed);
    wr(go[1], 'g');
    rd(done[0]);
    wait(NULL);
    kvspaceShmClose(kv);
    puts("PASS a15");
    return 0;
}

/* A16：Quiesce(0) 后既不 ack 也不声明 → 每轮放弃；kill 后恢复。 */
static int m_a16(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    churn(kv, v, l, 5);
    int ready[2];
    if (pipe(ready)) FAIL("pipe");
    pid_t p = fork();
    if (p == 0) {
        close(ready[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        quiesce_off(c);
        wr(ready[1], 'r');
        for (;;) pause();
    }
    close(ready[1]);
    rd(ready[0]);
    setenv("P2_BARRIER_MS", "50", 1);
    int rc = kvspaceShmViewSweep(kv);
    if (rc != -2) FAIL("silent-process sweep rc=%d want -2", rc);
    if (kvspaceShmViewGarbBytes(kv) == 0) FAIL("freed during abort");
    kill(p, SIGKILL);
    wait(NULL);
    int freed = kvspaceShmViewSweep(kv);
    if (freed != 5) FAIL("freed=%d want 5 after kill", freed);
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    kvspaceShmClose(kv);
    puts("PASS a16");
    return 0;
}

/* A19 补：reserve 后 alloc / art_ins 失败 → cancel；旧值不变、无残留、无泄漏。 */
static int m_a25(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l8 = 0, l512 = 0;
    uint8_t *v8 = mkval(8, &l8), *v512 = mkval(512, &l512);
    put(kv, "/k", v8, l8);
    setenv("P2_TEST_FAIL_ALLOC", "1", 1);
    if (kvspaceShmSetValue(kv, "/k", v512, (int32_t)l512, 0, 0) == 0)
        FAIL("alloc failure accepted");
    unsetenv("P2_TEST_FAIL_ALLOC");
    int32_t gl = 0;
    uint8_t *g = kvspaceShmGet(kv, "/k", 0, &gl);
    if (!g || gl != (int32_t)l8 || memcmp(g, v8, l8) != 0)
        FAIL("old value changed after alloc failure");
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("alloc failure left garbage");
    setenv("P2_TEST_FAIL_ARTINS", "1", 1);
    if (kvspaceShmSetValue(kv, "/k", v512, (int32_t)l512, 0, 0) == 0)
        FAIL("art_ins failure accepted");
    unsetenv("P2_TEST_FAIL_ARTINS");
    g = kvspaceShmGet(kv, "/k", 0, &gl);
    if (!g || gl != (int32_t)l8 || memcmp(g, v8, l8) != 0)
        FAIL("old value changed after art_ins failure");
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("art_ins failure left garbage");
    if (kvspaceShmViewSweep(kv) != 0) FAIL("residue after failed mutations");
    if (kvspaceShmSetValue(kv, "/k", v512, (int32_t)l512, 0, 0) != 0)
        FAIL("normal resize after failures");
    if (kvspaceShmViewSweep(kv) != 1) FAIL("normal resize must retire exactly 1");
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("garb_bytes not zero");
    kvspaceShmClose(kv);
    puts("PASS a25");
    return 0;
}

/* A27：leader 被 kill 且它的槽被新进程回收（旧 claim 彻底消失）→ 仍能接管。 */
static int m_a27(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    churn(kv, v, l, 5);
    int ready[2];
    if (pipe(ready)) FAIL("pipe");
    pid_t a = fork();
    if (a == 0) {
        close(ready[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        setenv("P2_TEST_HOLD_DRAIN_MS", "800", 1);
        wr(ready[1], 'r');
        kvspaceShmViewSweep(c);
        _exit(0);
    }
    close(ready[1]);
    rd(ready[0]);
    usleep(200 * 1000);
    kill(a, SIGKILL);
    waitpid(a, NULL, 0);
    /* B 在 A 死后才 open：注册表回收 A 的槽，旧 claim 不再存在 */
    pid_t b = fork();
    if (b == 0) {
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        int freed = kvspaceShmViewSweep(c);   /* 必须从 OWNED|dead-claim 接管 */
        _exit(freed == 4 ? 0 : 3);
    }
    int st = 0;
    waitpid(b, &st, 0);
    if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
        FAIL("takeover after slot recycle failed (st=%d)", st);
    if (kvspaceShmViewGarbBytes(kv) != 72)
        FAIL("leaked box accounting wrong: %llu",
             (unsigned long long)kvspaceShmViewGarbBytes(kv));
    if (kvspaceShmViewSweep(kv) != 0) FAIL("leaked entry resurfaced");
    kvspaceShmClose(kv);
    puts("PASS a27");
    return 0;
}

/* T3：活 owner 在跑时，另一个进程的 Sweep 必须立即返回 -1（不等待）。 */
static int m_t3(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    churn(kv, v, l, 5);
    int ready[2];
    if (pipe(ready)) FAIL("pipe");
    pid_t p = fork();
    if (p == 0) {
        close(ready[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        setenv("P2_TEST_HOLD_DRAIN_MS", "600", 1);
        wr(ready[1], 'r');
        kvspaceShmViewSweep(c);
        _exit(0);
    }
    close(ready[1]);
    rd(ready[0]);
    usleep(150 * 1000);                /* 子进程已在 drain 里持锁 */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = kvspaceShmViewSweep(kv);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    long ms = (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;
    if (rc != -1) FAIL("busy sweep rc=%d want -1", rc);
    if (ms > 100) FAIL("busy sweep waited %ldms", ms);
    wait(NULL);                        /* 子进程（leader）完成并释放 5 条 */
    if (kvspaceShmViewGarbBytes(kv) != 0) FAIL("leader did not finish");
    if (kvspaceShmViewSweep(kv) != 0) FAIL("nothing left to free");
    kvspaceShmClose(kv);
    puts("PASS t3");
    return 0;
}

/* A6：leader 在 drain 中 kill -9；下一轮接管，只漏一个 box，不 double free。 */
static int m_a6(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    churn(kv, v, l, 10);
    uint64_t cap = box_cap(kv);         /* 多一条 retired */
    uint64_t before = kvspaceShmViewGarbBytes(kv);
    int ready[2];
    if (pipe(ready)) FAIL("pipe");
    pid_t p = fork();
    if (p == 0) {
        close(ready[0]);
        kvspace_t *c = kvspaceShmOpen(store, STORE_SZ);
        if (!c) _exit(2);
        setenv("P2_TEST_HOLD_DRAIN_MS", "1000", 1);
        wr(ready[1], 'r');
        kvspaceShmViewSweep(c);         /* 卡在第一条 free 之前 */
        _exit(0);
    }
    close(ready[1]);
    rd(ready[0]);
    usleep(300 * 1000);
    kill(p, SIGKILL);
    wait(NULL);
    int freed = kvspaceShmViewSweep(kv);
    if (freed != 10) FAIL("freed=%d want 10 (one leaked by crash)", freed);
    uint64_t after = kvspaceShmViewGarbBytes(kv);
    if (after != cap) FAIL("garb_bytes=%llu want one box %llu",
                           (unsigned long long)after, (unsigned long long)cap);
    if (kvspaceShmViewSweep(kv) != 0) FAIL("second sweep must free 0");
    if (kvspaceShmViewGarbBytes(kv) != after) FAIL("leaked entry resurfaced");
    if ((uint64_t)before != cap * 11) FAIL("before=%llu want %llu",
                                           (unsigned long long)before,
                                           (unsigned long long)(cap * 11));
    kvspaceShmClose(kv);
    puts("PASS a6");
    return 0;
}

/* 持续制造垃圾的 helper（R4 括号测试用）：自身默认 quiesced=1，不阻塞别人的轮次。 */
static int m_churn(const char *store, int ms) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) FAIL("open");
    uint32_t l = 0;
    uint8_t *v = mkval(8, &l);
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        churn(kv, v, l, 200);
        clock_gettime(CLOCK_MONOTONIC, &t1);
        long long el = (t1.tv_sec - t0.tv_sec) * 1000 +
                       (t1.tv_nsec - t0.tv_nsec) / 1000000;
        if (el >= ms)
            break;
    }
    uint64_t ok = 0, ab = 0, lk = 0;
    kvspaceShmViewRoundStats(kv, &ok, &ab, &lk);
    printf("round_ok=%llu aborted=%llu leaks=%llu garb=%llu\n",
           (unsigned long long)ok, (unsigned long long)ab,
           (unsigned long long)lk,
           (unsigned long long)kvspaceShmViewGarbBytes(kv));
    kvspaceShmClose(kv);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) FAIL("usage: %s <mode> <store>", argv[0]);
    const char *m = argv[1], *st = argv[2];
    if (!strcmp(m, "t1")) return m_t1(st);
    if (!strcmp(m, "t4")) return m_t4(st);
    if (!strcmp(m, "t3")) return m_t3(st);
    if (!strcmp(m, "a25")) return m_a25(st);
    if (!strcmp(m, "churn"))
        return m_churn(st, argc > 3 ? atoi(argv[3]) : 3000);
    if (!strcmp(m, "a27")) return m_a27(st);
    if (!strcmp(m, "a19")) return m_a19(st);
    if (!strcmp(m, "a20")) return m_a20(st);
    if (!strcmp(m, "a13")) return m_a13(st, 0);
    if (!strcmp(m, "a14")) return m_a13(st, 1);
    if (!strcmp(m, "a15")) return m_a15(st);
    if (!strcmp(m, "a16")) return m_a16(st);
    if (!strcmp(m, "a6")) return m_a6(st);
    FAIL("unknown mode %s", m);
}
