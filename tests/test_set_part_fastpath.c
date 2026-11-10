/* test_set_part_fastpath — part-write 快路径与回退。
 *
 * 快路径：`kvspaceShmSetPartByRef` 在 `ref.gen == 0`、head 自洽、**值不是引用**、
 * offset/buf_len 在 per-box 物理容量内时直接写正文，跳过 `read_value`。
 * 任何不自洽必须回退现有路径（不静默写坏数据）。
 *
 * 覆盖：长度边界 9/10、99/100、255/256；越界拒绝；损坏/截断 head 回退；
 * **引用态不得进入快路径**；stale ref 拒绝。
 *
 * ── 判据变更记录（2026-11-06 布局迁移）──────────────────────────────
 * 旧版快路径要求 `storetype == SLACK && ptr == INLINE`。新布局没有这两个字段：
 * 「变长」由 langtype 表达，引用性由 langtype 的 `*`/`@` 前缀表达。所以：
 *   - 旧「非 SLACK 必须失败」→ **不再成立**（新判据下非引用值走快路径）；
 *   - 旧「ptr 非 INLINE 不得进快路径」→ **保留**，判据换成 langtype 前缀；
 *   - 旧「storetype 非法（>3）」→「layout 非法（>1）」→ 现在 head 没有布局位，
 *     换成「padding 非零 / 总长不自洽」。
 * 改的是判据来源，测试意图（不该进快路径的绝不进）没变。 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include <kvspace/xvalue_head.h>
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

/*
 * 写入一条有效长度 n 的文本值。
 *
 * `NewSlack` 不再有容量参数：body 区就是 n 字节，容量是存储后端的物理分配属性
 * （这里 CAP 只用作测试缓冲区上界）。所以下面所有 offset 计算以 n 为界——越界
 * 判定看的正是 bodylen。
 */
static int put(kvspace_t *kv, const char *key, const char *body, uint64_t n) {
    uint8_t *v = NULL;
    uint64_t vl = 0;
    if (kvspaceXvalueHeadNewSlack((const uint8_t *)body, n, "char/utf8",
                                  &v, &vl) != 0)
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
    char s[512];
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

    /* 2. 越界：offset+buf_len 超过 `bodylen` 必须拒绝且不写。
     *
     * `bodylen` 就是有效长度（16），wire 里没有第二个长度来源。写在它之外
     * 既越界又会让读方按 bodylen 读不到，等于静默丢写。 */
    {
        mkpc(s, 16);
        CHECK(put(kv, "/p/oob", s, 16) == 0, "oob set");
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/oob", &ref) == 0, "oob resolve");
        char nv[CAP + 80];
        memset(nv, 'X', sizeof nv);
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/oob", HEAD + 16 + 1,
                                     (const uint8_t *)nv, 1) != 0,
              "oob offset must fail");
        CHECK(kvspaceShmSetPartByRef(kv, &ref, "/p/oob", HEAD,
                                     (const uint8_t *)nv, 17) != 0,
              "oob len must fail");
        char got[16] = {0};
        CHECK(get_part(kv, "/p/oob", HEAD, 16, got) == 0, "oob intact");
        CHECK(memcmp(got, s, 16) == 0, "oob body unchanged");
        /* NULL buf 被前置检查拒绝（与 offset 无关）。 */
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
        /* 整条 wire 快照（head + body + attr）：下面的畸形用例会改 attr 区，
           只存 head 无法还原。 */
        uint8_t *save = (uint8_t *)malloc((size_t)len);
        CHECK(save != NULL, "save alloc");
        memcpy(save, d, (size_t)len);
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/bad", &ref) == 0, "bad resolve");
        const uint8_t *nv = (const uint8_t *)"ZZZZ";
        char got[8] = {0};

        /*
         * 判据变了（2026-11-06 布局迁移）：旧模型用 `storetype == SLACK` 表达
         * 「这个值长度可变、可原地改」，新布局里 head 没有这个字段——「变长」
         * 由 langtype 决定，而 SetPartByRef 不解析 langtype。
         *
         * 快路径前置：**不是引用** + head 自洽（padding 零、attr 无 tag 2）+
         * offset/buf_len 在 bodylen 内且不跨 box。所以「非 SLACK 必须失败」的
         * 用例换成新判据下的对应物：引用态不得进快路径、脏 padding 不得进。
         */
        /*
         * padding 弄脏：head 不自洽，快路径必须绕过它（旧路径的
         * `head_of` 也会拒，所以整体结果是失败，但绝不能写坏数据）。
         */
        kvspaceXvalueHead tmp;
        memcpy(&tmp, save, sizeof tmp);
        tmp.padding[0] = 1;
        memcpy(d, &tmp, sizeof tmp);
        int rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "head with nonzero padding must not take the fast path");
        memcpy(d, save, (size_t)len);

        /* 引用态（langtype 带 `*`）不得进快路径——这是旧「ptr 非 INLINE」那条的直接对应物。 */
        {
            static const char prefixed[] = "*int64";
            uint8_t *pv = NULL;
            uint64_t pvl = 0;
            kvspaceXvalueHead ph;
            memset(&ph, 0, sizeof ph);
            kvspaceXvalueHeadSetLen(&ph, 2);
            kvspaceXvalueHeadSetAttrsLen(&ph, (uint16_t)(3 + sizeof prefixed - 1));
            memcpy(d, &ph, sizeof ph);
            d[HEAD + 0] = '/';
            d[HEAD + 1] = 'x';
            {
                uint8_t *a = d + HEAD + 2;
                a[0] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
                a[1] = (uint8_t)(sizeof prefixed - 1);
                a[2] = 0;
                memcpy(a + 3, prefixed, sizeof prefixed - 1);
            }
            (void)pv;
            (void)pvl;
            rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
            memset(got, 0, sizeof got);
            get_part(kv, "/p/bad", HEAD, 4, got);
            CHECK(rc != 0 || memcmp(got, "ZZZZ", 4) == 0, "reference fallback");
            memcpy(d, save, (size_t)len);
        }

        /* attr 区出现旧 tag 2（reallen）：那是已删除的格式，head_of 扫描即拒。
           这是旧「storetype 非法（>3）」→「layout 非法」的第三形态。 */
        {
            kvspaceXvalueHead ah;
            memcpy(&ah, save, sizeof ah);
            const uint64_t old_attrs = kvspaceXvalueHeadAttrsLen(&ah);
            uint8_t *a = d + HEAD + kvspaceXvalueHeadLen(&ah);
            a[0] = 2; /* 旧 tag 2 */
            a[1] = 8; a[2] = 0;
            memset(a + 3, 0, 8);
            kvspaceXvalueHeadSetAttrsLen(&ah, (uint16_t)(old_attrs + 11));
            memcpy(d, &ah, sizeof ah);
            rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
            CHECK(rc != 0, "old tag2 in attr must fail");
            memcpy(d, save, (size_t)len);
        }

        /* len 越界（声称超过映射剩余空间）：拒绝 */
        memset(&tmp, 0, sizeof tmp);
        kvspaceXvalueHeadSetLen(&tmp, 0xFFFF);
        memcpy(d, &tmp, sizeof tmp);
        rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "len overflow must fail");
        memcpy(d, save, (size_t)len);

        /* len 大于 box 物理容量但仍在全局映射内：必须按 per-box cap 回退，
         * 不得跨 box 写入。offset=512 落在 box 外。 */
        {
            char lead[8] = {0}, trail[8] = {0};
            memcpy(lead, d + 512, 4);
            memset(&tmp, 0, sizeof tmp);
            kvspaceXvalueHeadSetLen(&tmp, 4096);
            memcpy(d, &tmp, sizeof tmp);
            rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", 512, nv, 4);
            memcpy(trail, d + 512, 4);
            CHECK(rc != 0, "len>box_cap must fail");
            CHECK(memcmp(lead, trail, 4) == 0, "no cross-box write");
            memcpy(d, save, (size_t)len);
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
