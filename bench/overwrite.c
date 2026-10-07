#define _GNU_SOURCE
#include "kvspace_shm.h"
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static uint64_t ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void put64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    int insert = !strcmp(argv[2], "insert-short") || !strcmp(argv[2], "insert-long");
    if (!insert && strcmp(argv[2], "update-one") && strcmp(argv[2], "update-128"))
        return 2;
    char *end = NULL;
    unsigned long count = strtoul(argv[3], &end, 10);
    if (*end || !count || count > UINT32_MAX) return 2;
    unsigned n = (unsigned)count;
    unsigned nk = insert ? n : !strcmp(argv[2], "update-one") ? 1 : 128;
    if (n < nk) return 2;
    void *dl = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!dl) { fprintf(stderr, "%s\n", dlerror()); return 3; }
    __typeof__(&kvspaceShmOpen) open_db = dlsym(dl, "kvspaceShmOpen");
    __typeof__(&kvspaceShmClose) close_db = dlsym(dl, "kvspaceShmClose");
    __typeof__(&kvspaceShmSetValue) set_value = dlsym(dl, "kvspaceShmSetValue");
    __typeof__(&kvspaceShmGet) get_value = dlsym(dl, "kvspaceShmGet");
    if (!open_db || !close_db || !set_value || !get_value) return 3;
    char (*keys)[96] = calloc(nk, sizeof(*keys));
    if (!keys) return 4;
    const char *format = !strcmp(argv[2], "insert-long")
        ? "/vthread/123456789/[12]/frame-value-many-common-characters/%08u"
        : "/keys/%08u";
    for (unsigned i = 0; i < nk; i++) snprintf(keys[i], 96, format, i);
    uint8_t value[40] = {5, 0};
    memcpy(value + 18, "int64", 5);
    char dir[] = "/tmp/kvspace-overwrite-XXXXXX";
    if (!mkdtemp(dir)) return 5;
    char db[256];
    snprintf(db, sizeof db, "%s/db", dir);
    kvspace_t *h = open_db(db, 8UL * 64 * 64 * 64 * 64);
    if (!h) return 6;
    if (!insert)
        for (unsigned i = 0; i < nk; i++)
            if (set_value(h, keys[i], value, 40, 0, 0) != 0) return 7;
    uint64_t start = ns();
    for (unsigned i = 0; i < n; i++) {
        put64(value + 32, i);
        if (set_value(h, keys[insert ? i : i % nk], value, 40, 0, 0) != 0) return 8;
    }
    uint64_t elapsed = ns() - start;
    for (unsigned j = 0; j < 16; j++) {
        unsigned k = (unsigned)(((uint64_t)j * (nk - 1)) / 15);
        uint64_t want = insert ? k : (n - 1) - ((n - 1 - k) % nk);
        int32_t len = 0;
        uint8_t *got = get_value(h, keys[k], 0, &len);
        put64(value + 32, want);
        if (!got || len != 40 || memcmp(value, got, 40) != 0) return 9;
    }
    close_db(h);
    free(keys);
    dlclose(dl);
    unlink(db);
    char side[300];
    snprintf(side, sizeof side, "%s.sbo.head", db); unlink(side);
    snprintf(side, sizeof side, "%s.sbo.data", db); unlink(side);
    rmdir(dir);
    printf("{\"case\":\"%s\",\"n\":%u,\"ns\":%llu,\"ns_per_set\":%.3f}\n",
           argv[2], n, (unsigned long long)elapsed, (double)elapsed / n);
    return 0;
}
