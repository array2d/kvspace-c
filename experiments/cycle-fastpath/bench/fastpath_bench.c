/* fastpath_bench.c — P0 精确基线：真实栈槽 key 的 5 类操作。
 *
 * 两份构建（同一源码）：
 *   (默认)           ABI 路径（kvspaceConnect + kvspace*），与 cycle-audit 口径一致
 *   -DFASTPATH_PROBE 原生 + 前缀探针（测 prefix+rel 的潜在成本）
 *
 * 用法: fastpath_bench <op> <key> <batches> <ops> <cal_n> <run> [prefix]
 *   op = get_hit | get_miss | set_over | set_new | del_hit | get_prefix
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef FASTPATH_PROBE
#include "kvspace_shm.h"
extern int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out,
                          uint32_t *out_len);
extern int32_t kvspaceProbeFindDir(void *h, const char *path, int32_t *skip);
extern uint8_t *kvspaceProbeGetRel(void *h, int32_t node, int32_t skip,
                                   const char *rel, int32_t *out_len);
#else
#include "kvspace/kvspace.h"
#endif

static inline uint64_t ticks(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("lfence\n\trdtscp\n\tlfence" : "=a"(lo), "=d"(hi) : : "rcx");
    return ((uint64_t)hi << 32) | lo;
}

static uint64_t g_cal_n = 100000;
static double calib(void) {
    static volatile uint64_t sink;
    uint64_t iters = g_cal_n / 8, best = UINT64_MAX;
    for (int k = 0; k < 5; k++) {
        uint64_t x = 1, t0 = ticks();
        for (uint64_t i = 0; i < iters; i++)
            __asm__ __volatile__("add $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\t"
                                 "add $1,%0\n\tadd $1,%0\n\tadd $1,%0\n\tadd $1,%0"
                                 : "+r"(x));
        uint64_t t1 = ticks();
        sink = x;
        if (t1 - t0 < best)
            best = t1 - t0;
    }
    return (double)(iters * 8) / (double)best;
}

static volatile uint64_t g_sink;
#define DIE(...)                                                    \
    do {                                                            \
        fprintf(stderr, "FATAL: " __VA_ARGS__);                     \
        fputc('\n', stderr);                                        \
        exit(2);                                                    \
    } while (0)

#ifdef FASTPATH_PROBE
typedef kvspace_t *H;
static H open_store(const char *path) {
    kvspace_t *kv = kvspaceShmOpen(path, 8ull * 64 * 64 * 64 * 64);
    if (!kv) DIE("open %s", path);
    return kv;
}
static void close_store(H h) { kvspaceShmClose(h); }
static int op_get(H h, const char *k, uint8_t **out, int32_t *len) {
    *out = kvspaceShmGet(h, k, 1, len);
    return *out && *len > 0 ? 0 : 1;
}
static int op_set(H h, const char *k, const uint8_t *v, uint32_t vl) {
    return kvspaceShmSetValue(h, k, v, (int32_t)vl, 0, 0);
}
static void op_del(H h, const char *k) { kvspaceShmDel(h, k); }
#else
typedef void *H;
static H open_store(const char *path) {
    char dsn[512];
    snprintf(dsn, sizeof dsn, "shm://%s", path);
    void *h = kvspaceConnect(dsn);
    if (!h) DIE("connect %s", dsn);
    return h;
}
static void close_store(H h) { kvspaceClose(h); }
static int op_get(H h, const char *k, uint8_t **out, int32_t *len) {
    uint32_t l = 0;
    int rc = kvspaceGet(h, k, 1, out, &l);
    *len = (int32_t)l;
    return rc == 0 && *out && l > 0 ? 0 : 1;
}
static int op_set(H h, const char *k, const uint8_t *v, uint32_t vl) {
    char err[256];
    return kvspaceSetValue(h, k, v, vl, 0, 0, err, sizeof err);
}
static void op_del(H h, const char *k) {
    char err[256];
    const char *kd[1] = {k};
    kvspaceDel(h, kd, 1, err, sizeof err);
}
#endif

int main(int argc, char **argv) {
    if (argc < 7)
        DIE("usage: %s op key batches ops cal_n run [prefix]", argv[0]);
    const char *op = argv[1];
    const char *key = argv[2];
    int batches = atoi(argv[3]);
    uint64_t ops = strtoull(argv[4], NULL, 10);
    g_cal_n = strtoull(argv[5], NULL, 10);
    int run = atoi(argv[6]);
    const char *prefix = argc > 7 ? argv[7] : NULL;
    const char *store = getenv("FASTPATH_STORE");
    if (!store)
        store = "/tmp/cycle-fastpath/shm/p0.dat";

    static uint8_t raw[8] = {'0', '1', '2', '3', '4', '5', '6', '7'};
    uint8_t *val = NULL;
    uint32_t val_len = 0;
    if (kvspaceNewChar(raw, 8, &val, &val_len) != 0)
        DIE("encode");

    H h = open_store(store);
    if (op_set(h, key, val, val_len) != 0)
        DIE("setup set %s", key);

    int32_t pnode = -1, pskip = 0;
    const char *rel = NULL;
    if (strcmp(op, "get_prefix") == 0) {
#ifdef FASTPATH_PROBE
        if (!prefix) DIE("get_prefix needs prefix");
        pnode = kvspaceProbeFindDir(h, prefix, &pskip);
        if (pnode < 0) DIE("prefix %s not found", prefix);
        rel = key + strlen(prefix);
        if (!rel[0]) DIE("empty rel");
#else
        DIE("get_prefix 需要 FASTPATH_PROBE 构建");
#endif
    }

    printf("# op=%s key=%s klen=%zu batches=%d ops=%" PRIu64 " cal_n=%" PRIu64
           " run=%d prefix=%s\n",
           op, key, strlen(key), batches, ops, g_cal_n, run, prefix ? prefix : "-");
    printf("op,run,batch,ops,ticks,ticks_per_op,cal_before,cal_after,drift_pct,"
           "core_cycles_per_op,checksum\n");

    for (int b = 0; b < batches; b++) {
        double ca = calib();
        uint64_t t0 = ticks(), sum = 0;
        if (strcmp(op, "get_hit") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                int32_t len = 0;
                uint8_t *out = NULL;
                if (op_get(h, key, &out, &len) != 0)
                    DIE("get_hit failed");
                sum += out[0] + out[len - 1];
            }
        } else if (strcmp(op, "get_prefix") == 0) {
#ifdef FASTPATH_PROBE
            for (uint64_t i = 0; i < ops; i++) {
                int32_t len = 0;
                uint8_t *out = kvspaceProbeGetRel(h, pnode, pskip, rel, &len);
                if (!out || len <= 0)
                    DIE("get_prefix failed");
                sum += out[0] + out[len - 1];
            }
#endif
        } else if (strcmp(op, "get_miss") == 0) {
            char miss[512];
            snprintf(miss, sizeof miss, "%s", key);
            size_t l = strlen(miss);
            miss[l - 1] = miss[l - 1] == 'z' ? 'y' : 'z';
            for (uint64_t i = 0; i < ops; i++) {
                int32_t len = 0;
                uint8_t *out = NULL;
                (void)op_get(h, miss, &out, &len);
                if (out || len)
                    DIE("get_miss returned value");
            }
        } else if (strcmp(op, "set_over") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                if (op_set(h, key, val, val_len) != 0)
                    DIE("set_over failed");
                sum += val[0];
            }
        } else if (strcmp(op, "set_new") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                op_del(h, key);
                if (op_set(h, key, val, val_len) != 0)
                    DIE("set_new failed");
                sum += val[0];
            }
        } else if (strcmp(op, "del_hit") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                if (op_set(h, key, val, val_len) != 0)
                    DIE("rebuild failed");
                uint64_t a = ticks();
                op_del(h, key);
                uint64_t c = ticks();
                sum += c - a;
            }
        } else {
            DIE("unknown op %s", op);
        }
        uint64_t t1 = ticks();
        double cb = calib();
        uint64_t dt = t1 - t0;
        double drift = (cb - ca) / ((cb + ca) * 0.5) * 100.0;
        g_sink += sum;
        double tpo = (strcmp(op, "del_hit") == 0) ? (double)sum / (double)ops
                                                  : (double)dt / (double)ops;
        printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%.3f,%.6f,%.6f,%.3f,%.3f,%" PRIu64
               "\n",
               op, run, b, ops, dt, tpo, ca, cb, drift, tpo * (ca + cb) * 0.5,
               (uint64_t)g_sink);
        fflush(stdout);
    }
    close_store(h);
    return 0;
}
