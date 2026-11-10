/*
 * benchmark-only 锚点钩子的自测：`kvspaceShmTestAnchorOf`。
 *
 * 验的是三条：
 *   1. 已存在父节点 → 能拿到 anchor，且这个 anchor **真能被 kvspaceResolve 用**
 *      （不是随便一个数：拿它去解析子键必须成功）；
 *   2. 不存在的父节点 → 失败，且 out_anchor 不被写；
 *   3. NULL 参数 → 失败。
 *
 * 为什么第 1 条要走到 kvspaceResolve：锚点的唯一用途就是当
 * `parent_key_anchor` 传回去。只断言「返回 0 且值非零」等于测了自己写的返回值，
 * 而没测它是否真的是一个可用锚点——那正是这个钩子存在的意义。
 */

/* mkdtemp 在 <stdlib.h> 中受 _DEFAULT_SOURCE 保护（glibc）：不定义它就走隐式声明，
 * 返回值按 int 截断——指针高 32 位被 cltq 填成 1，后面 strlen 直接崩。
 * 同类测试（test_art_alignment.c）也是这么开的。 */
#define _GNU_SOURCE

#include "kvspace/kvspace.h"
#include "kvspace_shm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);               \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int main(void) {
    char path[] = "/tmp/kvspace-anchor-hook-XXXXXX";
    char *dir = mkdtemp(path);
    if (!dir) {
        perror("mkdtemp");
        return 1;
    }
    char dsn[512];
    /* shm DSN 要指到具体的 store 文件前缀，只给目录连不上（与 test_storage_abi 同形）。 */
    snprintf(dsn, sizeof dsn, "shm://%s/db", dir);

    void *h = kvspaceConnect(dsn);
    if (!h) {
        fprintf(stderr, "FAIL: connect %s\n", dsn);
        return 1;
    }
    char err[256] = {0};
    kvspace_t *kv = (kvspace_t *)h;

    /* 空库直接建 /d/：它大概率落在 ART block 0，而 **block 0 是合法节点 id**。
     * 历史上 `storage_anchor_ok` 写成 `block_id <= 0` 把它误判成非法锚点，
     * 这条用例正是那个缺陷的回归判据——不需要任何 filler 键铺垫。 */
    /* 建索引 /d/ 与子项 /d/x——父节点有值槽，锚点才有意义。 */
    int rc = kvspaceMkindex(h, "/d/", err, sizeof err);
    CHECK(rc == KVSPACE_OK, "mkindex /d/ rc=%d err=%s", rc, err);
    uint64_t off = 0;
    rc = kvspaceRealloc(h, "/d/x", 8, 0, &off, err, sizeof err);
    CHECK(rc == KVSPACE_OK, "realloc /d/x rc=%d err=%s", rc, err);
    /* 填 8 字节 body，让值槽真正被占用。 */
    uint8_t *base = kvspaceXvalueInProcessAddress(h);
    CHECK(base != NULL, "in-process address must not be NULL for shm");
    if (base) {
        for (int i = 0; i < 8; i++)
            base[off + 16 + (uint64_t)i] = (uint8_t)(i + 1);
    }

    /* ① 已存在父节点：拿到 anchor，并用它解析子键。 */
    int64_t anchor = 0;
    rc = kvspaceShmTestAnchorOf(kv, "/d/", &anchor);
    CHECK(rc == KVSPACE_OK, "anchor_of /d/ rc=%d", rc);
    CHECK(anchor > 0, "anchor 必须是正的 key 区偏移，实际 %lld",
          (long long)anchor);

    uint64_t child_off = 0;
    memset(err, 0, sizeof err);
    rc = kvspaceResolve(h, "/d/x", (uint64_t)anchor, &child_off, err,
                        sizeof err);
    CHECK(rc == KVSPACE_OK,
          "用钩子给的 anchor 解析 /d/x 必须成功（rc=%d err=%s）—— "
          "否则这个 anchor 就不是可用锚点",
          rc, err);
    CHECK(child_off == off,
          "经 anchor 解析出的偏移应与 Realloc 返回的一致：%llu vs %llu",
          (unsigned long long)child_off, (unsigned long long)off);

    /* ② 不存在的父节点：失败，且不写 out_anchor。 */
    int64_t untouched = -12345;
    rc = kvspaceShmTestAnchorOf(kv, "/no-such-parent/", &untouched);
    CHECK(rc == KVSPACE_MISSING,
          "不存在的父节点必须 MISSING（rc=%d，期望 %d）", rc, KVSPACE_MISSING);
    CHECK(untouched == -12345, "失败时不得写 out_anchor，实际 %lld",
          (long long)untouched);

    /* ③ MOVED 节点：实现里有 `art_hdr(kv,id)->type == ART_MOVED` → BACKEND 的拒绝，
     * 但 MOVED 只在 ART 删除/压缩过程中瞬时出现，测试无法从公共 API 造出来。
     * 该路径靠代码评审与实现判据保证，这里不造伪状态——按 `report-three-states`
     * 的口径，没测到的就不假装测过。
     *
     * ④ NULL 参数。 */
    CHECK(kvspaceShmTestAnchorOf(NULL, "/d/", &anchor) == KVSPACE_INVALID,
          "NULL kv 必须 INVALID");
    CHECK(kvspaceShmTestAnchorOf(kv, NULL, &anchor) == KVSPACE_INVALID,
          "NULL key 必须 INVALID");
    CHECK(kvspaceShmTestAnchorOf(kv, "/d/", NULL) == KVSPACE_INVALID,
          "NULL out 必须 INVALID");

    kvspaceClose(h);
    /* 清理 shm 目录（后端会在其中建文件）。 */
    char cmd[600];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "warning: cleanup %s failed\n", dir);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("anchor_hook: all checks passed\n");
    return 0;
}
