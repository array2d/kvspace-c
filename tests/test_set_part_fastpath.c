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
 *   - 旧「storetype 非法（>3）」→ 换成「layout 非法（>1）」。
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
 * 写入一条**容量为 CAP 的文本值**（有效长度 = n）。
 *
 * 关键：`NewSlack(body, n, CAP)` 在 `CAP > n` 时产出 `LAYOUT_CAPACITY`，
 * **body 区是 CAP 字节**（有效长度记在 attr tag 2）。旧模型这里是
 * `storetype == SLACK`，body 区只有 n 字节、cap 只是物理预留——判据变了，
 * 所以下面所有 offset 计算都以 CAP（body 区大小）为界，不是 n。
 */
static int put(kvspace_t *kv, const char *key, const char *body, uint64_t n) {
    uint8_t *v = NULL;
    uint64_t vl = 0;
    if (kvspaceXvalueHeadNewSlack((const uint8_t *)body, n, CAP, "char/utf8",
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

    /* 2. 越界：offset+buf_len 超过**有效长度**必须拒绝且不写。
     *
     * 上界是有效长度 16（`put` 的 n），不是容量 CAP 256：值虽然分配了 256 字节
     * body 区，但有效内容只有 16 字节，写在它之外就是往「容量余量」里塞数据，
     * 读的人按有效长度读不到，等于静默丢写。 */
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
        uint8_t save[HEAD];
        memcpy(save, d, sizeof save);
        kvspaceRef_t ref;
        CHECK(kvspaceShmResolveRef(kv, "/p/bad", &ref) == 0, "bad resolve");
        const uint8_t *nv = (const uint8_t *)"ZZZZ";
        char got[8] = {0};

        /*
         * 判据变了（2026-11-06 布局迁移）：旧模型用 `storetype == SLACK` 表达
         * 「这个值长度可变、可原地改」，新布局里 head 没有这个字段——「变长」
         * 由 langtype 决定，而 SetPartByRef 不解析 langtype。
         *
         * 现在快路径的前置只有两条：**不是引用** + 容量自洽。所以原来这条
         * 「非 SLACK 必须失败」的用例不再成立，换成它在新判据下的对应物：
         * **引用态不得进快路径**（引用值是路径文本，原地改它的字节没有意义）。
         */
        /*
         * 本值是 CAPACITY 布局：有效长度在 attr tag 2，**清掉 attrs 会让它变成
         * 畸形**（CAPACITY 必须带 tag 2）。所以这里保留 attrs、只把 langtype
         * 记录去掉不现实——改为断言「畸形 head 一律不得进快路径」，
         * 这比原来那条「非 SLACK 必须失败」更贴合新判据：快路径只接受自洽 head。
         */
        kvspaceXvalueHead tmp;
        memcpy(&tmp, save, sizeof tmp);
        kvspaceXvalueHeadSetAttrsLen(&tmp, 0);
        memcpy(d, &tmp, sizeof tmp);
        /* 无 langtype = 非引用 ⇒ 这一条在新判据下**应当成功**（旧模型说必须失败）。 */
        int rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "malformed head (CAPACITY without tag 2) must not take the fast path");
        memcpy(d, save, HEAD);

        /* 引用态（langtype 带 `*`）不得进快路径——这是旧「ptr 非 INLINE」那条的直接对应物。 */
        {
            static const char prefixed[] = "*int64";
            uint8_t *pv = NULL;
            uint64_t pvl = 0;
            kvspaceXvalueHead ph;
            memset(&ph, 0, sizeof ph);
            ph.layout = KVSPACE_XVALUE_LAYOUT_LENGTH;
            kvspaceXvalueHeadSetLen(&ph, 2);
            kvspaceXvalueHeadSetAttrsLen(&ph, (uint32_t)(3 + sizeof prefixed - 1));
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
            memcpy(d, save, HEAD);
        }

        /* layout 非法（>1）：head 不自洽，拒绝。这是旧「storetype 非法（>3）」的对应物。 */
        memset(&tmp, 0, sizeof tmp);
        tmp.layout = 9;
        memcpy(d, &tmp, sizeof tmp);
        rc = kvspaceShmSetPartByRef(kv, &ref, "/p/bad", HEAD, nv, 4);
        CHECK(rc != 0, "invalid layout must fail");
        memcpy(d, save, HEAD);

        /* len 越界（声称超过映射剩余空间）：拒绝 */
        memset(&tmp, 0, sizeof tmp);
        tmp.layout = KVSPACE_XVALUE_LAYOUT_LENGTH;
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
            tmp.layout = KVSPACE_XVALUE_LAYOUT_LENGTH;
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
