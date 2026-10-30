/* ART 节点对齐与 store 格式版本守卫。
 *
 * 1) 每个 ART 块的数据起点 8 字节对齐，且 art_hdr_t.val（偏移 16）不跨 64B
 *    cache line。否则 8 字节原子字在 x86 上退化为 split lock，在 AArch64 上
 *    可能直接陷入（LSE/LLSC 要求自然对齐）——这是正确性前提，不只是性能。
 * 2) format_version 不是当前版本的 store 必须 open 失败（不迁移、不兼容）。
 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
    if (kvspaceXvalueHeadNewSlack( (const uint8_t *)s, strlen(s),
                          strlen(s), &v, n) != 0)
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

    /* 旧格式版本（任何非当前版本）：close 后 open 必须失败。 */
    kvspaceShmTestSetFormatVersion(kv, KVSPACE_FORMAT_VERSION - 1);
    kvspaceShmClose(kv);
    CHECK(kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64) == NULL);

    printf("PASS art_alignment\n");
    return 0;
}
