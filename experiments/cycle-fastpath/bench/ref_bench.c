/* ref_bench.c — v0.3.16 现有 ref/slotview API 的每操作成本。
 *
 * 用法: ref_bench <op> <key> <batches> <ops> <cal_n> <run>
 *   op = resolve | get_byref | get_byref_prefix | slotview_once | slotview_call
 *        | set_byref | setpart_byref | get_string
 */
#define _POSIX_C_SOURCE 200809L
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvspace/kvspace.h"

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

int main(int argc, char **argv) {
    if (argc < 7)
        DIE("usage: %s op key batches ops cal_n run", argv[0]);
    const char *op = argv[1];
    const char *key = argv[2];
    int batches = atoi(argv[3]);
    uint64_t ops = strtoull(argv[4], NULL, 10);
    g_cal_n = strtoull(argv[5], NULL, 10);
    int run = atoi(argv[6]);
    const char *store = getenv("REF_STORE");
    if (!store)
        store = "/tmp/cycle-fastpath/rebaseline/shm/ref.dat";

    static uint8_t raw[8] = {'0', '1', '2', '3', '4', '5', '6', '7'};
    uint8_t *val = NULL;
    uint32_t val_len = 0;
    if (kvspaceNewChar(raw, 8, &val, &val_len) != 0)
        DIE("encode");

    char dsn[512];
    snprintf(dsn, sizeof dsn, "shm://%s", store);
    void *h = kvspaceConnect(dsn);
    if (!h)
        DIE("connect");
    char err[256];
    if (kvspaceSetValue(h, key, val, val_len, 0, 0, err, sizeof err) != 0)
        DIE("setup set");

    kvspaceRef_t ref;
    if (kvspaceResolveRef(h, key, &ref) != 0)
        DIE("resolve");
    kvspaceRef_t pref = {ref.parent_id, ref.depth, ref.depth, ref.depth};

    kvspaceSlotView_t view = {0};
    int sv = kvspaceSlotView(h, &ref, key, &view);

    printf("# op=%s key=%s klen=%zu resolve{leaf=%u parent=%u depth=%u} slotview=%d "
           "vh=%" PRIu32 " body_cap=%" PRIu64 "\n",
           op, key, strlen(key), ref.block_id, ref.parent_id, ref.depth, sv,
           sv == 0 ? view.headlen : 0, sv == 0 ? view.body_cap : 0);
    printf("op,run,batch,ops,ticks,ticks_per_op,cal_before,cal_after,drift_pct,"
           "core_cycles_per_op,checksum\n");

    for (int b = 0; b < batches; b++) {
        double ca = calib();
        uint64_t t0 = ticks(), sum = 0;
        if (strcmp(op, "resolve") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                kvspaceRef_t r;
                if (kvspaceResolveRef(h, key, &r) != 0)
                    DIE("resolve failed");
                sum += r.block_id;
            }
        } else if (strcmp(op, "get_byref") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                uint8_t *out = NULL;
                uint32_t len = 0;
                if (kvspaceGetByRef(h, &ref, key, &out, &len) != 0 || !out || !len)
                    DIE("get_byref failed");
                sum += out[0] + out[len - 1];
            }
        } else if (strcmp(op, "get_byref_prefix") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                uint8_t *out = NULL;
                uint32_t len = 0;
                if (kvspaceGetByRef(h, &pref, key, &out, &len) != 0 || !out || !len)
                    DIE("get_byref_prefix failed");
                sum += out[0] + out[len - 1];
            }
        } else if (strcmp(op, "slotview_once") == 0) {
            if (sv != 0)
                DIE("slotview unsupported (%d)", sv);
            for (uint64_t i = 0; i < ops; i++) {
                sum += view.body[0] + view.body[7];
                view.body[0] = (uint8_t)i;
            }
        } else if (strcmp(op, "slotview_call") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                kvspaceSlotView_t v;
                if (kvspaceSlotView(h, &ref, key, &v) != 0)
                    DIE("slotview failed");
                sum += v.body[0];
            }
        } else if (strcmp(op, "set_byref") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                if (kvspaceSetValueByRef(h, &ref, key, val, val_len, 0, 0, err,
                                         sizeof err) != 0)
                    DIE("set_byref failed");
                sum += val[0];
            }
        } else if (strcmp(op, "setpart_byref") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                int rc = kvspaceSetPartByRef(h, &ref, key, (uint32_t)(getenv("REFPART_OFF") ? atoi(getenv("REFPART_OFF")) : (sv == 0 ? view.headlen : 64)), raw, 8, err, sizeof err);
                if (rc != 0)
                    DIE("setpart failed rc=%d err=%s", rc, err);
                sum += raw[0];
            }
        } else if (strcmp(op, "token_sim") == 0) {
            /* 模拟 P2 热路径：独立 cache line 上的 gen 校验 + seqlock + 读 body。
             * 不调用库；用来回答「旁路表多一次 load 会不会把 per-op 推过 50」。 */
            static uint32_t genbuf[64] __attribute__((aligned(64)));
            volatile uint32_t *vg = &genbuf[7];
            uint32_t sg = 3;             /* token.struct_gen */
            *vg = 4;                     /* 偶数 = 稳定 */
            for (uint64_t i = 0; i < ops; i++) {
                if (genbuf[6] != sg) { sum += 1; continue; }   /* struct_gen 校验 */
                uint32_t g1 = *vg;
                uint8_t v = view.body[0];
                uint32_t g2 = *vg;
                sum += v + ((g1 == g2) ? 0 : 2);
                view.body[0] = (uint8_t)i;
            }
        } else if (strcmp(op, "token_sim_set") == 0) {
            static uint32_t genbuf[64] __attribute__((aligned(64)));
            volatile uint32_t *vg = &genbuf[7];
            int alive = kvspaceSetPartByRef(h, &ref, key,
                     (uint32_t)(getenv("REFPART_OFF") ? atoi(getenv("REFPART_OFF")) : (sv == 0 ? view.headlen : 64)),
                     raw, 8, err, sizeof err);
            if (alive != 0 && alive != -1) { /* 允许首次失败，只测写路径 */ }
            for (uint64_t i = 0; i < ops; i++) {
                if (genbuf[6] != 3) { sum += 1; continue; }
                *vg = *vg + 1;           /* 奇 */
                memcpy(view.body, raw, 8); /* 8B 就地写 */
                *vg = *vg + 1;           /* 偶 */
                sum += view.body[0];
            }
        } else if (strcmp(op, "get_string") == 0) {
            for (uint64_t i = 0; i < ops; i++) {
                uint8_t *out = NULL;
                uint32_t len = 0;
                if (kvspaceGet(h, key, 1, &out, &len) != 0 || !out || !len)
                    DIE("get failed");
                sum += out[0] + out[len - 1];
            }
        } else {
            DIE("unknown op %s", op);
        }
        uint64_t t1 = ticks();
        double cb = calib();
        uint64_t dt = t1 - t0;
        double drift = (cb - ca) / ((cb + ca) * 0.5) * 100.0;
        g_sink += sum;
        double tpo = (double)dt / (double)ops;
        printf("%s,%d,%d,%" PRIu64 ",%" PRIu64 ",%.3f,%.6f,%.6f,%.3f,%.3f,%" PRIu64
               "\n",
               op, run, b, ops, dt, tpo, ca, cb, drift, tpo * (ca + cb) * 0.5,
               (uint64_t)g_sink);
        fflush(stdout);
    }
    kvspaceClose(h);
    return 0;
}
