/*
 * 01_basic — Set, Get, List, Del
 *
 * # expected:
 * # === Set & Get ===
 * # /t01/a	int64:42
 * # === Set & List ===
 * # a	int64
 * # b	int64
 * # c	string
 * # === Get bulk ===
 * # /t01/a	int64:42
 * # /t01/b	int64:7
 * # /t01/c	string:hello
 * # === Get nil ===
 * # /t01/nonexist	(nil)
 * # === Del ===
 * # /t01/a	(nil)
 * # b	int64
 * # c	string
 * # /end
 */
#include "kvspace/kvspace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void get(kvspace_t *kv, const char *key) {
    int32_t len; uint8_t *v = kvspaceShmGet(kv, key, 1, &len);
    if (!v || len == 0) { printf("%s\t(nil)\n", key); return; }
    xvalue_head_t h = xvalue_decode_head(v, len);
    printf("%s\t%.*s:", key, h.kind_len, h.kind);
    if (h.raw_len > 0) {
        if (strncmp(h.kind, XK_INT64, h.kind_len) == 0) printf("%ld", xvalue_at_int64(&h, 0));
        else if (strncmp(h.kind, XK_STRING, h.kind_len) == 0) printf("%.*s", h.raw_len, h.raw);
        else if (strncmp(h.kind, XK_FLOAT64, h.kind_len) == 0) printf("%.2f", xvalue_at_float64(&h, 0));
    }
    printf("\n");
    free(v);
}

static void set_int(kvspace_t *kv, const char *key, int64_t v) {
    uint8_t *b; int32_t bl = xvalue_new_int64_1(v, &b);
    kvspaceShmSet(kv, key, b, bl); free(b);
}
static void set_str(kvspace_t *kv, const char *key, const char *v) {
    uint8_t *b; int32_t bl = xvalue_new_char(v, &b);
    kvspaceShmSet(kv, key, b, bl); free(b);
}
static void list(kvspace_t *kv, const char *dir, bool show_kind) {
    char **ns; int32_t nc;
    kvspaceShmList(kv, dir, false, 1, &ns, &nc);
    for (int i = 0; i < nc; i++) {
        char *k = malloc(strlen(dir) + strlen(ns[i]) + 2);
        sprintf(k, "%s%s", dir, ns[i]);
        if (show_kind) {
            int32_t len; uint8_t *v = kvspaceShmGet(kv, k, 1, &len);
            if (v) { xvalue_head_t h = xvalue_decode_head(v, len);
                printf("%s\t%.*s", ns[i], h.kind_len, h.kind);
                if (h.raw_len > 0 && strncmp(h.kind, XK_INT64, h.kind_len) == 0)
                    printf("\t%ld", xvalue_at_int64(&h, 0));
                printf("\n"); free(v);
            }
        } else {
            printf("%s\n", ns[i]);
        }
        free(k);
    }
    for (int i = 0; i < nc; i++) free(ns[i]); free(ns);
}

int main() {
    const char *p = "/tmp/kvspace_t01.shm"; unlink(p);
    kvspace_t *kv = kvspaceShmOpen(p, 512);
    if (!kv) return 1;
    kvspaceShmMkindex(kv, "/t01/");

    printf("=== Set & Get ===\n");
    set_int(kv, "/t01/a", 42);
    get(kv, "/t01/a");

    printf("=== Set & List ===\n");
    set_int(kv, "/t01/b", 7);
    set_str(kv, "/t01/c", "hello");
    list(kv, "/t01/", true);

    printf("=== Get bulk ===\n");
    get(kv, "/t01/a"); get(kv, "/t01/b"); get(kv, "/t01/c");

    printf("=== Get nil ===\n");
    get(kv, "/t01/nonexist");

    printf("=== Del ===\n");
    kvspaceShmDel(kv, "/t01/a");
    get(kv, "/t01/a");
    list(kv, "/t01/", false);

    kvspaceShmClose(kv); unlink(p);
    return 0;
}
