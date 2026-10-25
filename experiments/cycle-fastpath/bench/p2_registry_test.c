/* p2_registry_test.c — 注册表/屏障三例（Linux）：
 *   slots   <store>                64 槽占满，第 65 次 open 必须失败且错误明确
 *   orphan  <store>                子进程持 pin 后 kill -9 → sweep 屏障回收墓碑 box
 *   sigstop <store>                存活进程 SIGSTOP 不 ack → 屏障超时不回收；恢复后 Check STALE
 * 退出码 0 = 全部断言通过。 */
#define _GNU_SOURCE
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "kvspace_shm.h"

extern int kvspaceNewChar(const uint8_t *, uint32_t, uint8_t **, uint32_t *);
extern int kvspaceShmViewResolve(kvspace_t *, const char *, kvspaceViewToken_t *);
extern int kvspaceShmViewCheck(kvspace_t *, const kvspaceViewToken_t *);
extern int kvspaceShmViewDel(kvspace_t *, kvspaceViewToken_t *, const char *);
extern int kvspaceShmViewRelease(kvspace_t *, kvspaceViewToken_t *);
extern int kvspaceShmViewSweep(kvspace_t *);

#define STORE_SZ (8ull * 64 * 64 * 64 * 64)

static int open_kv(const char *store) {
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    return kv ? 0 : -1;
}

/* ── slots ─────────────────────────────────────────────────────── */
static int t_slots(const char *store) {
    enum { HOLD = 64 };
    /* 先由父进程创建 store，避免 64 个 fork 同时 O_EXCL 创建的竞争（与槽无关）。 */
    kvspace_t *seed = kvspaceShmOpen(store, STORE_SZ);
    if (!seed) return 2;
    kvspaceShmClose(seed);
    int made[2];
    if (pipe(made) != 0) return 2;
    pid_t kids[HOLD];
    for (int i = 0; i < HOLD; i++) {
        pid_t p = fork();
        if (p == 0) {
            close(made[0]);
            kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
            char c = kv ? '1' : '0';
            (void)!write(made[1], &c, 1);
            if (!kv) _exit(3);
            for (;;) pause();
        }
        if (p < 0) return 2;
        kids[i] = p;
    }
    close(made[1]);
    int held = 0;
    for (int i = 0; i < HOLD; i++) {
        char c = 0;
        if (read(made[0], &c, 1) == 1 && c == '1') held++;
    }
    close(made[0]);
    int ok = (held == HOLD);
    kvspace_t *extra = kvspaceShmOpen(store, STORE_SZ);
    if (ok) ok = (extra == NULL);        /* 65th 必须失败 */
    if (extra) { kvspaceShmClose(extra); ok = 0; }
    for (int i = 0; i < HOLD; i++) kill(kids[i], SIGKILL);
    for (int i = 0; i < HOLD; i++) waitpid(kids[i], NULL, 0);
    printf("slots: held=%d/%d extra_open=%s => %s\n", held, HOLD,
           extra ? "SUCCEEDED(bug)" : "rejected(ok)", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── 公共：子进程 pin 墓碑；SIGCONT 后校验 token 是否 STALE 再退出 ── */
static void on_cont(int sig) { (void)sig; }

static pid_t pin_child(const char *store, const char *key, int rfd, int wfd) {
    pid_t p = fork();
    if (p != 0) return p;
    close(rfd);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_cont;
    sigaction(SIGCONT, &sa, NULL);
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    kvspaceViewToken_t t = {0};
    int rc = kv ? kvspaceShmViewResolve(kv, key, &t) : -1;
    char c = (rc == 0) ? '1' : '0';
    (void)!write(wfd, &c, 1);
    if (rc != 0) _exit(4);
    pause();                            /* SIGSTOP 期间不返回；SIGCONT 经 handler 打断 */
    int chk = kvspaceShmViewCheck(kv, &t);
    char r = (chk != 0) ? 'S' : 'F';    /* S = STALE（期望），F = 仍在读（bug） */
    (void)!write(wfd, &r, 1);
    _exit(chk != 0 ? 0 : 5);
}

static int wait_char(int fd) {
    char c = 0;
    return (read(fd, &c, 1) == 1 && c == '1') ? 0 : -1;
}

/* ── orphan ────────────────────────────────────────────────────── */
static int t_orphan(const char *store) {
    const char *key = "/vthread/1/[9]/o";
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) return 2;
    uint8_t raw[8] = {'A'}, *v = NULL;
    uint32_t vl = 0;
    kvspaceNewChar(raw, 8, &v, &vl);
    if (kvspaceShmSetValue(kv, key, v, (int32_t)vl, 0, 0) != 0) return 2;

    int a[2];
    if (pipe(a) != 0) return 2;
    pid_t kid = pin_child(store, key, a[0], a[1]);
    close(a[1]);
    if (kid < 0 || wait_char(a[0]) != 0) return 2;
    close(a[0]);
    /* 子进程已 pin：父进程删除成墓碑，再 kill -9 持 pin 的 owner */
    kvspaceViewToken_t t = {0};
    if (kvspaceShmViewResolve(kv, key, &t) != 0) return 2;
    if (kvspaceShmViewDel(kv, &t, key) != 0) return 2;
    kvspaceShmViewRelease(kv, &t);
    kill(kid, SIGKILL);
    waitpid(kid, NULL, 0);
    int freed = kvspaceShmViewSweep(kv);
    kvspaceShmClose(kv);
    int ok = freed > 0;
    printf("orphan: sweep freed=%d (expect >0) => %s\n", freed, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

/* ── sigstop ───────────────────────────────────────────────────── */
static int t_sigstop(const char *store) {
    const char *ka = "/vthread/1/[8]/s";
    const char *kb = "/vthread/1/[8]/d";
    kvspace_t *kv = kvspaceShmOpen(store, STORE_SZ);
    if (!kv) return 2;
    uint8_t raw[8] = {'A'}, *v = NULL;
    uint32_t vl = 0;
    kvspaceNewChar(raw, 8, &v, &vl);
    if (kvspaceShmSetValue(kv, ka, v, (int32_t)vl, 0, 0) != 0 ||
        kvspaceShmSetValue(kv, kb, v, (int32_t)vl, 0, 0) != 0)
        return 2;
    int a[2], b[2];
    if (pipe(a) != 0 || pipe(b) != 0) return 2;
    pid_t pa = pin_child(store, ka, a[0], a[1]);
    close(a[1]);
    pid_t pb = pin_child(store, kb, b[0], b[1]);
    close(b[1]);
    if (pa < 0 || pb < 0 || wait_char(a[0]) != 0 || wait_char(b[0]) != 0)
        return 2;
    close(b[0]);            /* a[0] 留到恢复后读 Check 结果 */
    /* A 删成墓碑后 SIGSTOP；B 删成墓碑后 kill -9（触发屏障） */
    kvspaceViewToken_t t = {0};
    if (kvspaceShmViewResolve(kv, ka, &t) != 0) return 2;
    kvspaceShmViewDel(kv, &t, ka);
    kvspaceShmViewRelease(kv, &t);
    if (kvspaceShmViewResolve(kv, kb, &t) != 0) return 2;
    kvspaceShmViewDel(kv, &t, kb);
    kvspaceShmViewRelease(kv, &t);
    kill(pa, SIGSTOP);
    kill(pb, SIGKILL);
    waitpid(pb, NULL, 0);
    setenv("P2_BARRIER_MS", "50", 1);
    int freed1 = kvspaceShmViewSweep(kv);      /* A 不 ack → 超时，不回收 A 的 box */
    kill(pa, SIGCONT);
    char r = 0;
    (void)!read(a[0], &r, 1);                  /* A 恢复后首次 Check 必须 STALE */
    close(a[0]);
    int st = 0;
    waitpid(pa, &st, 0);
    int stale_ok = (r == 'S');
    kvspaceShmClose(kv);

    kvspace_t *kv2 = kvspaceShmOpen(store, STORE_SZ);
    int freed2 = kvspaceShmViewSweep(kv2);     /* A 已退出 → 屏障完成 → 回收 */
    kvspaceShmClose(kv2);
    int ok = (freed1 == 0) && stale_ok && (freed2 > 0);
    printf("sigstop: timeout_sweep freed=%d (expect 0), resume_check=%c (expect S), "
           "post-exit freed=%d (expect >0) => %s\n",
           freed1, r ? r : '?', freed2, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s slots|orphan|sigstop <store>\n", argv[0]);
        return 2;
    }
    const char *mode = argv[1], *store = argv[2];
    char p[512];
    snprintf(p, sizeof p, "%s", store);
    if (strcmp(mode, "slots") == 0) return t_slots(p);
    if (strcmp(mode, "orphan") == 0) return t_orphan(p);
    if (strcmp(mode, "sigstop") == 0) return t_sigstop(p);
    fprintf(stderr, "unknown mode %s\n", mode);
    return 2;
}
