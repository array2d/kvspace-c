#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%d: %s\n", __LINE__, #c); return 1; } } while (0)

static int same(kvspace_t *kv, const char *key, const uint8_t *value, uint64_t n) {
    int32_t len = 0;
    uint8_t *got = kvspaceShmGet(kv, key, 0, &len);
    return got && len == (int32_t)n && memcmp(got, value, (size_t)n) == 0;
}

int main(void) {
    char dir[] = "/tmp/kvspace-value-refs-XXXXXX", path[128];
    CHECK(mkdtemp(dir));
    snprintf(path, sizeof path, "%s/db", dir);
    kvspace_t *a = kvspaceShmOpen(path, 8UL * 64 * 64);
    kvspace_t *b = kvspaceShmOpen(path, 8UL * 64 * 64);
    CHECK(a && b);
    uint8_t *value = NULL, raw[4096] = {42};
    uint64_t n = 0;
    CHECK(kvspaceXhNewScalar("int64", raw, 8, &value, &n) == 0);
    CHECK(kvspaceShmSet(a, "/unrelated", value, (int32_t)n) == 0);
    CHECK(kvspaceShmSet(a, "/local/k", value, (int32_t)n) == 0);
    free(value);
    kvspaceRef_t ref;
    CHECK(kvspaceShmResolveRef(a, "/local/k", &ref) == 0);
    const uint64_t sizes[] = {1, 8, 32, 4096, 3, 512, 1};
    for (unsigned i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        raw[0] = (uint8_t)(i + 50);
        CHECK(kvspaceXhNewSlack(1, raw, sizes[i], sizes[i], &value, &n) == 0);
        CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 1, i + 1) == 0);
        CHECK(same(b, "/local/k", value, n));
        uint8_t ro;
        uint32_t vid;
        CHECK(kvspaceShmMetaGetAt(b, "/local/k", &ro, &vid) == 0 && ro == 1 && vid == i + 1);
        CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 0, 0) == 0);
        CHECK(kvspaceShmMetaGetAt(b, "/local/k", &ro, &vid) == 0 && ro == 0 && vid == 0);
        free(value);
    }
    uint64_t dims[] = {2, 2};
    CHECK(kvspaceXhNewTensor(dims, 2, "float64", raw, 32, &value, &n) == 0);
    CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 0, 0) == 0);
    CHECK(same(b, "/local/k", value, n));
    free(value);
    CHECK(kvspaceXhNewPtr("float64", "/unrelated", 10, &value, &n) == 0);
    CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 0, 0) == 0);
    CHECK(same(b, "/local/k", value, n));
    free(value);
    CHECK(kvspaceXhNewScalar("int64", raw, 8, &value, &n) == 0);
    CHECK(kvspaceShmSet(b, "/local/k", value, (int32_t)n) == 0);
    for (int i = 0; i < 60; i++) {
        char key[64];
        snprintf(key, sizeof key, "/local/k%c", i + 32);
        CHECK(kvspaceShmSet(b, key, value, (int32_t)n) == 0);
    }
    CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 0, 0) == 0);
    CHECK(same(b, "/local/k", value, n));
    uint8_t invalid[3] = {31};
    CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", invalid, sizeof invalid, 0, 0) != 0);
    CHECK(same(b, "/local/k", value, n));
    CHECK(kvspaceShmDel(b, "/local/k") == 0);
    CHECK(kvspaceShmMkindex(b, "/remote/", 0) == 0);
    CHECK(kvspaceShmSet(b, "/remote/k", value, (int32_t)n) == 0);
    CHECK(kvspaceShmExtindex(b, "/local/", "/remote/") == 0);
    for (int i = 0; i < 3; i++) {
        int32_t len = 0;
        uint8_t *got = kvspaceShmGetByRef(a, &ref, "/local/k", &len);
        CHECK(got && len == (int32_t)n && memcmp(got, value, (size_t)n) == 0);
    }
    CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 0, 0) != 0);
    CHECK(kvspaceShmDelextindex(b, "/local/") == 0);
    CHECK(kvspaceShmSetValueByRef(a, &ref, "/local/k", value, (int32_t)n, 0, 0) == 0);
    CHECK(same(b, "/local/k", value, n));
    free(value);
    kvspaceShmClose(b);
    kvspaceShmClose(a);
    return 0;
}
