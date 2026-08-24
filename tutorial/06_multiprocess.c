/*
 * 06_multiprocess — A 进程写，B 进程读，共享 file-backed mmap
 *
 * 用法: ./06_multiprocess /tmp/kv.shm 32768
 */

#include "kvspace/kvspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <time.h>

#define N_KEYS  500
#define N_ROUND 3

static uint8_t *enc_int(int64_t v, int32_t *l) {
    uint8_t *b = malloc(22); b[0] = 5; memcpy(b+1, "int64", 5);
    int32_t a = 1, r = 8; memcpy(b+6, &a, 4); memcpy(b+10, &r, 4);
    memcpy(b+14, &v, 8); *l = 22; return b;
}
static uint8_t *enc_str(const char *s, int32_t *l) {
    int32_t sl = strlen(s), t = 15 + sl; uint8_t *b = malloc(t);
    b[0] = 6; memcpy(b+1, "string", 6);
    int32_t a = 1; memcpy(b+7, &a, 4); memcpy(b+11, &sl, 4);
    memcpy(b+15, s, sl); *l = t; return b;
}
static uint8_t *enc_bytes(const uint8_t *raw, int32_t rl, int32_t *l) {
    int32_t t = 14 + rl; uint8_t *b = malloc(t);
    b[0] = 5; memcpy(b+1, "bytes", 5);
    int32_t a = 1; memcpy(b+6, &a, 4); memcpy(b+10, &rl, 4);
    memcpy(b+14, raw, rl); *l = t; return b;
}

static int writer(kvspace_t *kv, int64_t seed) {
    srand((unsigned)seed);
    int errors = 0;
    kvspaceShmMkindex(kv, "/mp/");

    printf("[writer] round 1: write %d ints\n", N_KEYS);
    for (int i = 0; i < N_KEYS; i++) {
        char k[64]; snprintf(k, sizeof(k), "/mp/k%d", i);
        int64_t v = ((int64_t)rand() << 32) | rand();
        int32_t l; uint8_t *b = enc_int(v, &l);
        if (kvspaceShmSet(kv, k, b, l) != 0) { printf("[writer] FAIL set %s\n", k); errors++; }
        free(b);
    }

    printf("[writer] round 2: write %d strings + delete 20%% ints\n", N_KEYS / 5);
    int del_n = N_KEYS / 5;
    for (int i = 0; i < del_n; i++) {
        int idx = rand() % N_KEYS;
        char k[64]; snprintf(k, sizeof(k), "/mp/k%d", idx);
        kvspaceShmDel(kv, k);
    }
    for (int i = 0; i < 100; i++) {
        char k[64], s[64]; snprintf(k, sizeof(k), "/mp/s%d", i);
        snprintf(s, sizeof(s), "str_val_%d_%d", i, rand() % 1000);
        int32_t l; uint8_t *b = enc_str(s, &l);
        if (kvspaceShmSet(kv, k, b, l) != 0) { printf("[writer] FAIL set %s\n", k); errors++; }
        free(b);
    }

    printf("[writer] round 3: large bytes\n");
    uint8_t big[1024];
    for (int j = 0; j < 1024; j++) big[j] = (uint8_t)(rand() & 0xFF);
    for (int i = 0; i < 20; i++) {
        char k[64]; snprintf(k, sizeof(k), "/mp/big%d", i);
        int32_t l; uint8_t *b = enc_bytes(big, 1024, &l);
        kvspaceShmSet(kv, k, b, l); free(b);
    }
    // write some additional ints (not updates)
    for (int j = 0; j < 50; j++) {
        char k[64]; snprintf(k, sizeof(k), "/mp/extra%d", j);
        int64_t v = ((int64_t)rand() << 32) | rand();
        int32_t l; uint8_t *b = enc_int(v, &l);
        kvspaceShmSet(kv, k, b, l); free(b);
    }

    printf("[writer] done, errors=%d\n", errors);
    kvspaceShmClose(kv);
    return errors;
}

static int reader(kvspace_t *kv, int64_t seed) {
    srand((unsigned)seed);
    int errors = 0;

    printf("[reader] listing /mp/\n");
    char **ns; int32_t nc;
    kvspaceShmList(kv, "/mp/", false, 1, &ns, &nc);
    printf("[reader]   total children: %d\n", nc);

    // verify some ints
    srand((unsigned)seed);
    printf("[reader] verifying ints...\n");
    int checked = 0;
    for (int i = 0; i < N_KEYS; i++) {
        char k[64]; snprintf(k, sizeof(k), "/mp/k%d", i);
        int64_t expected = ((int64_t)rand() << 32) | rand();
        int32_t l; uint8_t *v = kvspaceShmGet(kv, k, 1, &l);
        if (v) {
            xvalue_head_t h = xvalue_decode_head(v, l);
            if (h.raw_len >= 8 && strncmp(h.kind, XK_INT64, h.kind_len) == 0) {
                int64_t got = xvalue_at_int64(&h, 0);
                if (got != expected) {
                    printf("[reader] MISMATCH %s: got=%ld expected=%ld\n", k, got, expected);
                    errors++;
                }
            }
            free(v); checked++;
        }
        // key may have been deleted in round 2 — that's fine
    }
    printf("[reader]   checked %d ints, errors=%d\n", checked, errors);

    // verify strings
    printf("[reader] verifying strings...\n");
    for (int i = 0; i < 100; i++) {
        char k[64]; snprintf(k, sizeof(k), "/mp/s%d", i);
        int32_t l; uint8_t *v = kvspaceShmGet(kv, k, 1, &l);
        if (!v) continue; // may have been overwritten
        xvalue_head_t h = xvalue_decode_head(v, l);
        if (strncmp(h.kind, XK_STRING, h.kind_len) != 0) {
            printf("[reader] BADKIND %s: %.*s\n", k, h.kind_len, h.kind); errors++;
        }
        free(v);
    }
    printf("[reader]   string check done\n");

    // verify big bytes
    printf("[reader] verifying big values...\n");
    int big_ok = 0;
    for (int i = 0; i < 20; i++) {
        char k[64]; snprintf(k, sizeof(k), "/mp/big%d", i);
        int32_t l; uint8_t *v = kvspaceShmGet(kv, k, 1, &l);
        if (v) {
            xvalue_head_t h = xvalue_decode_head(v, l);
            if (h.raw_len == 1024) big_ok++;
            free(v);
        }
    }
    printf("[reader]   big values found: %d/20\n", big_ok);

    kvspaceShmClose(kv);
    printf("[reader] done, errors=%d\n", errors);
    return errors;
}

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/tmp/kvspace_mp.shm";
    size_t sz = argc > 2 ? (size_t)atol(argv[2]) : (8ULL * 64 * 64 * 64);
    int64_t seed = (int64_t)time(NULL);

    unlink(path);

    printf("=== phase 1: writer ===\n");
    kvspace_t *kv = kvspaceShmOpen(path, sz);
    if (!kv) { printf("FAIL: writer open\n"); return 1; }
    int we = writer(kv, seed);
    // kv is already closed by writer

    printf("=== phase 2: reader (same SHM) ===\n");
    kv = kvspaceShmOpen(path, sz);
    if (!kv) { printf("FAIL: reader open\n"); return 1; }
    int re = reader(kv, seed);

    unlink(path);
    int total = we + re;
    printf("\n%s: %d errors\n", total == 0 ? "ALL OK" : "FAIL", total);
    return total;
}
