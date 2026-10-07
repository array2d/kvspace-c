#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VALUES 10000
#define SBO_DATA_SIZE (8UL * 64 * 64 * 64 * 64)

static double now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + t.tv_nsec;
}

static void free_names(char **names, int32_t count) {
    for (int32_t i = 0; i < count; i++)
        free(names[i]);
    free(names);
}

static int call(kvspace_t *kv, int length_only, int expected) {
    int32_t count = -1;
    if (length_only)
        return kvspaceShmListLen(kv, "/bench/", false, 0, &count) == 0 &&
               count == expected ? 0 : -1;
    char **names = NULL;
    int rc = kvspaceShmList(kv, "/bench/", false, 0, &names, &count);
    free_names(names, count > 0 ? count : 0);
    return rc == 0 && count == expected ? 0 : -1;
}

static double time_calls(kvspace_t *kv, int length_only, int expected,
                         uint64_t reps) {
    double start = now_ns();
    for (uint64_t i = 0; i < reps; i++)
        if (call(kv, length_only, expected) != 0)
            return -1;
    return now_ns() - start;
}

int main(int argc, char **argv) {
    if (argc != 4) {
        fprintf(stderr, "usage: %s STORE CASE List|ListLen\n", argv[0]);
        return 2;
    }
    int groups, tombstones = 0, values = VALUES, name_width = 5;
    if (strcmp(argv[2], "flat") == 0)
        groups = VALUES;
    else if (strcmp(argv[2], "ten-children") == 0)
        groups = 10;
    else if (strcmp(argv[2], "one-child") == 0)
        groups = 1;
    else if (strcmp(argv[2], "long-child") == 0) {
        groups = values = 1;
        name_width = 900;
    } else if (strcmp(argv[2], "long-flat") == 0) {
        groups = VALUES;
        name_width = 900;
    } else if (strcmp(argv[2], "last-live") == 0) {
        groups = 1;
        tombstones = VALUES - 1;
    } else if (strcmp(argv[2], "all-dead") == 0) {
        groups = 1;
        tombstones = VALUES;
    } else
        return 2;
    int length_only = strcmp(argv[3], "ListLen") == 0;
    if (!length_only && strcmp(argv[3], "List") != 0)
        return 2;
    kvspace_t *kv = kvspaceShmOpen(argv[1], SBO_DATA_SIZE);
    if (!kv)
        return 1;
    uint8_t value[4] = {1}, *encoded = NULL;
    uint64_t size = 0;
    if (kvspaceXhNewScalar("int32", value, sizeof value, &encoded, &size) != 0)
        return 1;
    for (int i = 0; i < values; i++) {
        char key[1024];
        if (groups == VALUES || name_width == 900)
            snprintf(key, sizeof key, "/bench/%0*d", name_width, i);
        else
            snprintf(key, sizeof key, "/bench/%05d/deep/%05d/value",
                     i % groups, i);
        if (kvspaceShmSet(kv, key, encoded, (int32_t)size) != 0 ||
            (i < tombstones && kvspaceShmDel(kv, key) != 0))
            return 1;
    }
    free(encoded);
    int expected = tombstones == VALUES ? 0 : groups;
    char **names = NULL;
    int32_t count = 0;
    if (kvspaceShmList(kv, "/bench/", false, 0, &names, &count) != 0 ||
        count != expected)
        return 1;
    for (int i = 0; i < count; i++) {
        char name[1024];
        snprintf(name, sizeof name, "%0*d", name_width, i);
        if (strcmp(names[i], name) != 0)
            return 1;
    }
    free_names(names, count);
    for (int i = 0; i < 20; i++)
        if (call(kv, length_only, expected) != 0)
            return 1;
    uint64_t reps = 1;
    double elapsed;
    do {
        elapsed = time_calls(kv, length_only, expected, reps);
        if (elapsed < 0)
            return 1;
        if (elapsed >= 3e7)
            break;
        reps *= 2;
    } while (reps < (UINT64_C(1) << 30));
    reps = (uint64_t)((double)reps * 1.2e8 / elapsed) + 1;
    elapsed = time_calls(kv, length_only, expected, reps);
    if (elapsed < 0)
        return 1;
    printf("%s,%s,%" PRIu64 ",%.3f,%d\n", argv[2], argv[3], reps,
           elapsed / (double)reps, expected);
    kvspaceShmClose(kv);
    return 0;
}
