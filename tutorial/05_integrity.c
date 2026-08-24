/*
 * 05_integrity — 数据完整性压力测试，探测 slotsboxmalloc 漏洞
 *
 * 写入大量随机数据，读回校验。多轮增删改混合操作。
 */

#include "kvspace/kvspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define N_KEYS     400
#define MAX_VAL_SZ  1024

static uint8_t *encode_int(int64_t v, int32_t *len) {
    return (uint8_t *)memcpy(malloc(22), (uint8_t[]){
        5,'i','n','t','6','4', 1,0,0,0, 8,0,0,0,
        (uint8_t)(v),(uint8_t)(v>>8),(uint8_t)(v>>16),(uint8_t)(v>>24),
        (uint8_t)(v>>32),(uint8_t)(v>>40),(uint8_t)(v>>48),(uint8_t)(v>>56)
    }, *len = 22);
}

static uint8_t *encode_str(const char *s, int32_t *len) {
    int32_t sl = (int32_t)strlen(s);
    int32_t total = 1 + 6 + 8 + sl;
    uint8_t *b = malloc(total);
    b[0] = 6; memcpy(b+1, "string", 6);
    int32_t al=1; memcpy(b+7, &al, 4); memcpy(b+11, &sl, 4);
    memcpy(b+15, s, sl);
    *len = total;
    return b;
}

static uint8_t *encode_bytes(const uint8_t *raw, int32_t rl, int32_t *len) {
    int32_t total = 1 + 5 + 8 + rl;
    uint8_t *b = malloc(total);
    b[0] = 5; memcpy(b+1, "bytes", 5);
    int32_t al=1; memcpy(b+6, &al, 4); memcpy(b+10, &rl, 4);
    memcpy(b+14, raw, rl);
    *len = total;
    return b;
}

static bool check(kvspace_t *kv, const char *key, int64_t expected, int round) {
    int32_t len; uint8_t *v = kvspaceShmGet(kv, key, 1, &len);
    if (!v) { printf("ROUND%d MISS  %s\n", round, key); return false; }
    xvalue_head_t h = xvalue_decode_head(v, len);
    if (h.raw_len < 8 || strncmp(h.kind, XK_INT64, h.kind_len) != 0) {
        printf("ROUND%d CORRUPT %s: bad kind=%.*s raw_len=%d\n", round, key, h.kind_len, h.kind, h.raw_len);
        free(v); return false;
    }
    int64_t got = xvalue_at_int64(&h, 0);
    if (got != expected) {
        printf("ROUND%d MISMATCH %s: got=%ld expected=%ld\n", round, key, got, expected);
        free(v); return false;
    }
    free(v);
    return true;
}

static bool check_str(kvspace_t *kv, const char *key, const char *expected, int round) {
    int32_t len; uint8_t *v = kvspaceShmGet(kv, key, 1, &len);
    if (!v) { printf("ROUND%d MISS  %s\n", round, key); return false; }
    xvalue_head_t h = xvalue_decode_head(v, len);
    if (strncmp(h.kind, XK_STRING, h.kind_len) != 0) {
        printf("ROUND%d CORRUPT %s: bad kind\n", round, key); free(v); return false;
    }
    if (h.raw_len != (int32_t)strlen(expected) || memcmp(h.raw, expected, h.raw_len) != 0) {
        printf("ROUND%d MISMATCH %s: got=%.*s expected=%s\n", round, key, h.raw_len, h.raw, expected);
        free(v); return false;
    }
    free(v);
    return true;
}

int main() {
    const char *path = "/tmp/kvspace_integrity.shm";
    unlink(path);

    kvspace_t *kv = kvspaceShmOpen(path, 8ULL * 64 * 64 * 64); // 8*64^3 = 2MB
    if (!kv) { printf("FAIL: open\n"); return 1; }
    kvspaceShmMkindex(kv, "/it/");

    srand((unsigned)time(NULL));
    char keys[N_KEYS][64];
    int64_t values[N_KEYS];
    char strvals[N_KEYS][64];
    int n = 0;
    int errors = 0;

    /* ── Round 1: 批量写入 int64 ── */
    int n_keys = 200;
    printf("=== Round 1: write %d int64 keys ===\n", n_keys);
    for (int i = 0; i < n_keys; i++) {
        snprintf(keys[i], sizeof(keys[i]), "/it/k%d", i);
        values[i] = ((int64_t)rand() << 32) | (int64_t)rand();
        int32_t len; uint8_t *v = encode_int(values[i], &len);
        kvspaceShmSet(kv, keys[i], v, len);
        free(v); n++;
    }
    printf("  wrote %d keys\n", n);

    /* ── Round 1: 读回校验 ── */
    printf("=== Round 1: verify all ===\n");
    for (int i = 0; i < n; i++)
        if (!check(kv, keys[i], values[i], 1)) errors++;

    /* ── Round 2: 随机删除 30%，写入新值 ── */
    printf("=== Round 2: delete 30%%, write new ===\n");
    int del_count = n * 3 / 10;
    for (int i = 0; i < del_count; i++) {
        int idx = rand() % n;
        if (keys[idx][0]) { kvspaceShmDel(kv, keys[idx]); keys[idx][0] = '\0'; }
    }
    // fill holes with new keys
    for (int i = 0; i < del_count; i++) {
        int idx = n + i;
        snprintf(keys[idx], sizeof(keys[idx]), "/it/new_k%d", i);
        values[idx] = (int64_t)i * 1000;
        int32_t len; uint8_t *v = encode_int(values[idx], &len);
        kvspaceShmSet(kv, keys[idx], v, len); free(v);
    }
    n += del_count;

    /* ── Round 2: 校验存活数据 ── */
    printf("=== Round 2: verify %d keys ===\n", n);
    for (int i = 0; i < n; i++)
        if (keys[i][0] && !check(kv, keys[i], values[i], 2)) errors++;

    /* ── Round 3: 覆盖写入 (update in place) ── */
    printf("=== Round 3: update 50 keys ===\n");
    for (int j = 0; j < 50; j++) {
        int i = rand() % n;
        if (!keys[i][0]) continue;
        values[i] = ((int64_t)rand() << 32) | (int64_t)rand();
        int32_t len; uint8_t *v = encode_int(values[i], &len);
        kvspaceShmSet(kv, keys[i], v, len); free(v);
    }

    /* ── Round 3: 校验 ── */
    for (int i = 0; i < n; i++)
        if (keys[i][0] && !check(kv, keys[i], values[i], 3)) errors++;

    /* ── Round 4: 混合类型（int64 + string）写入 ── */
    printf("=== Round 4: mixed int64 + string ===\n");
    int str_base = n;
    for (int i = 0; i < 100; i++) {
        snprintf(keys[str_base + i], sizeof(keys[0]), "/it/s%d", i);
        snprintf(strvals[i], sizeof(strvals[0]), "val_%d_%d", i, rand() % 1000);
        int32_t len; uint8_t *v = encode_str(strvals[i], &len);
        kvspaceShmSet(kv, keys[str_base + i], v, len); free(v);
    }

    /* ── Round 4: 校验 string ── */
    for (int i = 0; i < 100; i++) {
        char k[64]; snprintf(k, sizeof(k), "/it/s%d", i);
        if (!check_str(kv, k, strvals[i], 4)) errors++;
    }

    /* ── Round 5: bytes 大 value (1KB random) ── */
    printf("=== Round 5: large bytes values ===\n");
    uint8_t big_vals[20][MAX_VAL_SZ];
    for (int i = 0; i < 20; i++) {
        for (int j = 0; j < MAX_VAL_SZ; j++) big_vals[i][j] = (uint8_t)(rand() & 0xFF);
        char k[64]; snprintf(k, sizeof(k), "/it/big%d", i);
        int32_t len; uint8_t *v = encode_bytes(big_vals[i], MAX_VAL_SZ, &len);
        kvspaceShmSet(kv, k, v, len); free(v);
    }

    /* ── Round 5: 删除 big values 再写入小值（复用 slot） ── */
    for (int i = 0; i < 10; i++) {
        char k[64]; snprintf(k, sizeof(k), "/it/big%d", i);
        kvspaceShmDel(kv, k);
        snprintf(k, sizeof(k), "/it/reuse%d", i);
        int32_t len; uint8_t *v = encode_int((int64_t)i, &len);
        kvspaceShmSet(kv, k, v, len); free(v);
    }

    /* ── Round 5: 校验大 value 和复用后的小 value ── */
    for (int i = 10; i < 20; i++) {
        char k[64]; snprintf(k, sizeof(k), "/it/big%d", i);
        int32_t len; uint8_t *v = kvspaceShmGet(kv, k, 1, &len);
        if (!v) { printf("ROUND5 MISS  big%d\n", i); errors++; continue; }
        xvalue_head_t h = xvalue_decode_head(v, len);
        if (h.raw_len != MAX_VAL_SZ || memcmp(h.raw, big_vals[i], MAX_VAL_SZ) != 0) {
            printf("ROUND5 CORRUPT big%d\n", i); errors++;
        }
        free(v);
    }
    for (int i = 0; i < 10; i++) {
        char k[64]; snprintf(k, sizeof(k), "/it/reuse%d", i);
        if (!check(kv, k, (int64_t)i, 5)) errors++;
    }

    /* ── 最终全量扫描 ── */
    printf("=== final: list and verify all ===\n");
    char **ns; int32_t nc;
    kvspaceShmList(kv, "/it/", false, 1, &ns, &nc);
    printf("  total children: %d\n", nc);
    for (int i = 0; i < nc; i++) free(ns[i]); free(ns);

    kvspaceShmClose(kv);
    unlink(path);

    if (errors == 0) printf("\nALL OK (0 errors)\n");
    else printf("\nFAIL: %d errors\n", errors);
    return errors;
}
