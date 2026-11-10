/* ART 节点对齐与 store 格式版本守卫。
 *
 * 1) 每个 ART 块的数据起点 8 字节对齐，且 art_hdr_t.val（偏移 16）不跨 64B
 *    cache line。否则 8 字节原子字在 x86 上退化为 split lock，在 AArch64 上
 *    可能直接陷入（LSE/LLSC 要求自然对齐）——这是正确性前提，不只是性能。
 * 2) Opening a store whose format_version != current version must fail (no
 *    migration or backward compatibility) and print a rebuild hint to stderr.
 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include <kvspace/kvlang_xvalue.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "failed: %s (line %d)\n", #x, __LINE__);           \
            return 1;                                                          \
        }                                                                      \
    } while (0)

static uint8_t *slack(const char *s, uint64_t *n) {
    uint8_t *v = NULL;
    if (kvspaceXvalueHeadNewSlack((const uint8_t *)s, strlen(s), "", &v, n) != 0)
        return NULL;
    return v;
}

int main(void) {
    char dir[] = "/tmp/kvspace-art-align-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/db", dir);

    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    CHECK(kv != NULL);

    /* 建出不同长度的键 → 不同尺寸的 ART 节点。 */
    for (int i = 0; i < 64; i++) {
        char key[64];
        snprintf(key, sizeof key, "/a/%0*d", 1 + i % 7, i);
        uint64_t n = 0;
        uint8_t *v = slack("x", &n);
        CHECK(v != NULL);
        CHECK(kvspaceShmSet(kv, key, v, (int32_t)n) == 0);
        free(v);
    }

    CHECK(kvspaceShmArtAlignCheck(kv) == -1);

    /* Older format version (any non-current version): reopen after close must fail
     * and print a rebuild hint to stderr (checked before mapping full current header). */
    kvspaceShmTestSetFormatVersion(kv, KVSPACE_FORMAT_VERSION - 1);
    kvspaceShmClose(kv);
    /* A real format-5 store header had no list_gen (8 bytes shorter), making the
     * ART file 8 bytes shorter as well; mapping the current header first would fail
     * inside region_attach before reaching a post-mmap version check.
     *
     * 注意：下面这个 8 是 **store header 的 v4→v5 字段差**（缺一个 list_gen），
     * 与 XValue head 的长度毫无关系。不要把它换成 KVSPACE_XVALUE_HEAD_LEN 或任何
     * head 宏——那会把对的改成错的。 */
    struct stat st;
    CHECK(stat(path, &st) == 0 && truncate(path, st.st_size - 8) == 0);
    char errpath[160];
    snprintf(errpath, sizeof errpath, "%s/stderr", dir);
    fflush(stderr);
    int saved = dup(STDERR_FILENO);
    FILE *ef = fopen(errpath, "w+");
    CHECK(saved >= 0 && ef != NULL);
    dup2(fileno(ef), STDERR_FILENO);
    kvspace_t *old = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    CHECK(old == NULL);
    char msg[512] = {0};
    rewind(ef);
    size_t ml = fread(msg, 1, sizeof msg - 1, ef);
    fclose(ef);
    msg[ml] = 0;
    char want[64];
    snprintf(want, sizeof want, "version=%u", (unsigned)(KVSPACE_FORMAT_VERSION - 1));
    if (!strstr(msg, "rebuild") || !strstr(msg, want))
        fprintf(stderr, "open stderr: [%s]\n", msg);
    CHECK(strstr(msg, "rebuild") != NULL);
    CHECK(strstr(msg, want) != NULL);

    printf("PASS art_alignment\n");
    return 0;
}
