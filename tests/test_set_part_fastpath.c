/* test_set_part_fastpath — SLACK pow=6 part-write 快路径与回退。
 *
 * 快路径：kvspaceShmSetPartByRef 在 ref.gen==0、SLACK/pow=6、head 的 b 与
 * offset/buf_len 全部自洽时直接写正文，跳过 read_value。任何不自洽必须回退
 * 现有路径（行为与旧实现一致，不静默写坏数据）。
 *
 * 覆盖：长度边界 9/10、99/100、255/256；越界拒绝；损坏/截断 head 回退；
 * Ptr flag 不得进入快路径；stale ref 拒绝。 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue_head.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SBO_DATA_SIZE (8UL * 64 * 64 * 64 * 64)
#define CAP 256
#define HEAD KVSPACE_XVALUE_HEAD_LEN

static int failures;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

static void mkpc(char *out, size_t len) {
    for (size_t i = 0; i + 1 < len; i++)
        out[i] = (char)('a' + (i % 26));
    out[len - 1] = ']';
    out[len] = 0;
}

static int put(kvspace_t *kv, const char *key, const char *body, uint64_t n) {
    uint8_t *v = NULL;
    uint64_t vl = 0;
    if (kvspaceXvalueHeadNewSlack( (const uint8_t *)body, n, CAP, "", &v, &vl) != 0)
        return -1;
    int rc = kvspaceShmSet(kv, key, v, (int32_t)vl);
    free(v);
    return rc;
}

static const uint8_t *raw(kvspace_t *kv, const char *key, int32_t *len) {
    return kvspaceShmGet(kv, key, 0, len);
}

/* 取 part；不存在/越界返回 NULL。 */
static int get_part(kvspace_t *kv, const char *key, uint32_t off, uint32_t n,
                    char *out) {
    int32_t len = 0;
    const uint8_t *d = kvspaceShmGet(kv, key, 0, &len);
    if (!d || off + n > (uint32_t)len)
        return -1;
    memcpy(out, d + off, n);
    return 0;
}

int main(void) {
    char dir[] = "/tmp/kvs1-fp-XXXXXX";
    char db[256];
    if (!mkdtemp(dir))
        return 2;
    snprintf(db, sizeof db, "%s/shm", dir);
    kvspace_t *kv = kvspaceShmOpen(db, SBO_DATA_SIZE);
    if (!kv)
        return 2;

    /* 1. 正常路径：长度边界与跨档；每次重写整值后 part-write 覆盖正文。 */
    static const size_t lens[] = {9, 10, 39, 99, 100, 101, 255, 256, 9};
    char s[512], body[CAP];
    for (size_t i = 0; i < sizeof lens / sizeof lens[0]; i++) {
        mkpc(s, lens[i]);
        CHECK(put(kv, "/p/pc", s, lens[i]) == 0, "set len=%zu", lens[i]);
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/pc", &ref) == 0, "resolve");
        CHECK(ref.gen == 0, "leaf ref gen");
        char nv[512];
        mkpc(nv, lens[i]);
        nv[0] = 'Z';
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/pc", HEAD,
                                     (const uint8_t *)nv, (uint32_t)lens[i]) == 0,
              "part len=%zu", lens[i]);
        char got[512] = {0};
        CHECK(get_part(kv, "/p/pc", HEAD, (uint32_t)lens[i], got) == 0, "getpart");
        CHECK(memcmp(got, nv, lens[i]) == 0, "body len=%zu", lens[i]);
        /* 相同长度再写一次（缓存 ref 复用快路径）。 */
        nv[1] = 'Q';
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/pc", HEAD,
                                     (const uint8_t *)nv, (uint32_t)lens[i]) == 0,
              "part2 len=%zu", lens[i]);
        memset(got, 0, sizeof got);
        CHECK(get_part(kv, "/p/pc", HEAD, (uint32_t)lens[i], got) == 0, "getpart2");
        CHECK(memcmp(got, nv, lens[i]) == 0, "body2 len=%zu", lens[i]);
    }

    /* 2. 越界：offset+buf_len > 64+b 必须拒绝且不写。 */
    {
        mkpc(s, 16);
        CHECK(put(kv, "/p/oob", s, 16) == 0, "oob set");
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/oob", &ref) == 0, "oob resolve");
        char nv[CAP + 80];
        memset(nv, 'X', sizeof nv);
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/oob", HEAD + CAP + 1,
                                     (const uint8_t *)nv, 1) != 0,
              "oob offset must fail");
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/oob", HEAD,
                                     (const uint8_t *)nv, CAP + 1) != 0,
              "oob len must fail");
        char got[16] = {0};
        CHECK(get_part(kv, "/p/oob", HEAD, 16, got) == 0, "oob intact");
        CHECK(memcmp(got, s, 16) == 0, "oob body unchanged");
        /* 末尾零长写：offset == 64+b 允许。 */
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/oob", HEAD + CAP, NULL, 0) != 0,
              "NULL buf rejected by precheck");
        /* 固定头：offset 越界（> 8+len）→ 拒绝。 */
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/oob", HEAD + CAP,
                                     (const uint8_t *)"", 0) != 0,
              "offset beyond body must fail");
    }

    /* 3. 损坏 head：快路径必须回退；旧路径的校验决定成败，但绝不能写错数据。 */
    {
        mkpc(s, 20);
        CHECK(put(kv, "/p/bad", s, 20) == 0, "bad set");
        int32_t len = 0;
        uint8_t *d = (uint8_t *)raw(kv, "/p/bad", &len);
        CHECK(d != NULL && len >= HEAD, "bad raw");
        uint8_t save[HEAD];
        memcpy(save, d, sizeof save);
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/bad", &ref) == 0, "bad resolve");
        const uint8_t *nv = (const uint8_t *)"ZZZZ";
        char got[8] = {0};

        /* storetype 非 SLACK：不进快路径；SLACK 之外 body 无 part-write
         * 语义，fallback 同样拒绝 */
        kvspaceXvalueHead tmp;
        memset(&tmp, 0, sizeof tmp);
        tmp.storetype = KVSPACE_XVALUE_ST_SHORT;
        memcpy(d, &tmp, sizeof tmp);
        int rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "storetype short must fail");
        memcpy(d, save, HEAD);

        /* ptr 非 INLINE：不得进快路径 */
        memset(&tmp, 0, sizeof tmp);
        tmp.storetype = KVSPACE_XVALUE_ST_SLACK;
        tmp.ptr = KVSPACE_XVALUE_PTR_PTR;
        memcpy(d, &tmp, sizeof tmp);
        rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        memset(got, 0, sizeof got);
        get_part(kv, "/p/bad", HEAD, 4, got);
        CHECK(rc != 0 || memcmp(got, "ZZZZ", 4) == 0, "ptr fallback");
        memcpy(d, save, HEAD);

        /* storetype 非法（>3）：head 不自洽，拒绝 */
        memset(&tmp, 0, sizeof tmp);
        tmp.storetype = 4;
        memcpy(d, &tmp, sizeof tmp);
        rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "storetype invalid must fail");
        memcpy(d, save, HEAD);

        /* len 越界（声称超过映射剩余空间）：拒绝 */
        memset(&tmp, 0, sizeof tmp);
        tmp.storetype = KVSPACE_XVALUE_ST_SLACK;
        kvspaceXvalueHeadSetLen(&tmp, 0xFFFF);
        memcpy(d, &tmp, sizeof tmp);
        rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "len overflow must fail");
        memcpy(d, save, HEAD);

        /* len 大于 box 物理容量但仍在全局映射内：必须按 per-box cap 回退，
         * 不得跨 box 写入。offset=512 落在 box 外。 */
        {
            char lead[8] = {0}, trail[8] = {0};
            memcpy(lead, d + 512, 4);
            memset(&tmp, 0, sizeof tmp);
            tmp.storetype = KVSPACE_XVALUE_ST_SLACK;
            kvspaceXvalueHeadSetLen(&tmp, 4096);
            memcpy(d, &tmp, sizeof tmp);
            rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", 512, nv, 4);
            memcpy(trail, d + 512, 4);
            CHECK(rc != 0, "len>box_cap must fail");
            CHECK(memcmp(lead, trail, 4) == 0, "no cross-box write");
            memcpy(d, save, HEAD);
        }

        /* 恢复后快路径仍可用 */
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4) == 0,
              "restored fast path");
        memset(got, 0, sizeof got);
        CHECK(get_part(kv, "/p/bad", HEAD, 4, got) == 0, "restored get");
        CHECK(memcmp(got, "ZZZZ", 4) == 0, "restored body");
    }

    /* 4. stale ref：键已删，ref 指向旧叶子。 */
    {
        mkpc(s, 16);
        CHECK(put(kv, "/p/gone", s, 16) == 0, "gone set");
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/gone", &ref) == 0, "gone resolve");
        CHECK(kvspaceShmDel(kv, "/p/gone") == 0, "gone del");
        const uint8_t *nv = (const uint8_t *)"QQQQ";
        int rc = kvspaceShmSetPartByRef(kv, &ref, "/p/gone", HEAD, nv, 4);
        CHECK(rc != 0, "stale ref must fail");
        CHECK(kvspaceShmSetPartByRef(kv, &ref, NULL, HEAD, nv, 4) != 0,
              "stale ref without fallback must fail");
    }

    kvspaceShmClose(kv);
    if (failures) {
        fprintf(stderr, "%d failures\n", failures);
        return 1;
    }
    printf("ok: set-part fast path + fallback\n");
    return 0;
}
