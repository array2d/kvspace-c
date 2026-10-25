/*
 * 01_basic — Set / Get / List / Del（统一 kvspace* ABI，shm:// 后端）
 *
 * # expected:
 * # === Set & Get ===
 * # /t01/a	int64:42
 * # === Set & List ===
 * # a	int64
 * # b	int64
 * # c	char/utf8
 * # === Get bulk ===
 * # /t01/a	int64:42
 * # /t01/b	int64:7
 * # /t01/c	char/utf8:hello
 * # === Get nil ===
 * # /t01/nonexist	(nil)
 * # === Del ===
 * # /t01/a	(nil)
 * # b	int64
 * # c	char/utf8
 * # /end
 */
#include "common.h"
#include <stdio.h>

static void get(void *kv, const char *key) {
    uint32_t len; uint8_t *v = kv_get(kv, key, &len);
    if (!v || len == 0) { printf("%s\t(nil)\n", key); return; }
    char kind[32]; const uint8_t *body; uint32_t bl;
    dec(v, len, kind, sizeof(kind), &body, &bl);
    printf("%s\t%s:", key, kind);
    if (strcmp(kind, "int64") == 0 && bl >= 8) printf("%ld", *(const int64_t *)body);
    else if (strcmp(kind, "char/utf8") == 0) printf("%.*s", (int)bl, body);
    else if (strcmp(kind, "float64") == 0 && bl >= 8) printf("%.2f", *(const double *)body);
    printf("\n");
    free(v);
}

static void set_int(void *kv, const char *key, int64_t v) {
    uint32_t l; uint8_t *b = enc_int64(v, &l); kv_set(kv, key, b, l); free(b);
}
static void set_str(void *kv, const char *key, const char *s) {
    uint32_t l; uint8_t *b = enc_str(s, &l); kv_set(kv, key, b, l); free(b);
}
static void list(void *kv, const char *dir, int show_kind) {
    uint32_t len; uint8_t *out = NULL;
    kv_list(kv, dir, &out, &len);
    if (!out || len == 0) return;
    uint32_t i = 0;
    while (i < len) {
        uint32_t j = i;
        while (j < len && out[j] != '\n') j++;
        char name[256];
        uint32_t n = j - i;
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, out + i, n); name[n] = 0;
        if (show_kind) {
            char full[512]; snprintf(full, sizeof(full), "%s%s", dir, name);
            uint32_t vl; uint8_t *v = kv_get(kv, full, &vl);
            if (v) { char kind[32]; const uint8_t *body; uint32_t bl;
                dec(v, vl, kind, sizeof(kind), &body, &bl);
                printf("%s\t%s", name, kind);
                if (strcmp(kind, "int64") == 0 && bl >= 8) printf("\t%ld", *(const int64_t *)body);
                printf("\n");
                free(v);
            }
        } else {
            printf("%s\n", name);
        }
        i = j + 1;
    }
    free(out);
}

int main(void) {
    const char *path = "/tmp/kvspace_t01.shm";
    remove(path);
    void *kv = kv_open_shm(path);
    if (!kv) return 1;
    kvspaceMkindex(kv, "/t01/", NULL, 0);

    printf("=== Set & Get ===\n");
    set_int(kv, "/t01/a", 42);
    get(kv, "/t01/a");

    printf("=== Set & List ===\n");
    set_int(kv, "/t01/b", 7);
    set_str(kv, "/t01/c", "hello");
    list(kv, "/t01/", 1);

    printf("=== Get bulk ===\n");
    get(kv, "/t01/a"); get(kv, "/t01/b"); get(kv, "/t01/c");

    printf("=== Get nil ===\n");
    get(kv, "/t01/nonexist");

    printf("=== Del ===\n");
    kv_del(kv, "/t01/a");
    get(kv, "/t01/a");
    list(kv, "/t01/", 0);

    kvspaceClose(kv);
    remove(path);
    return 0;
}
