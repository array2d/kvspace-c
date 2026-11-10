/* test_storage_abi — 存储层 ABI（kvspace/include/kvspace/kvspace.h）的 shm 实现。
 *
 * 覆盖：
 *   1. realloc 不存在则创建；容量内原地（返回同一偏移）；
 *   2. realloc 超容量搬移（返回新偏移，旧偏移失效）；
 *   3. free 删值、删空索引；非空索引 HAS_CHILDREN；
 *   4. listlen 的 N/M、非索引 NOT_INDEX、空索引 0/0；
 *   5. list 容量不足 CAPACITY 且仍写所需值；out_names=NULL 只探测；
 *      generation 随 key 集合变化前进、值修改不前进；
 *   6. makeindex 幂等，且不覆盖上层写过的内容。
 *
 * 本文件**只** include storage.h：与旧头混在同一 TU 会撞
 * kvspaceListLen/kvspaceListlen 这类只差大小写的声明。
 *
 * 只走新 ABI 能观测到的量（偏移、长度、个数、generation、errno），
 * **不**解引用 value_offset —— 新头没暴露「本进程基址」，这是契约缺口，
 * 已单独报告；补上基址入口后再加字节级断言。
 */
#include <kvspace/kvspace.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SBO_DATA_SIZE (8UL * 64 * 64 * 64 * 64)

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

int main(void) {
    char dir[] = "/tmp/kvspace-storage-abi-XXXXXX";
    if (!mkdtemp(dir))
        return 2;
    char db[256];
    snprintf(db, sizeof db, "shm://%s/db", dir);
    void *h = kvspaceConnect(db);
    CHECK(h != NULL, "connect");
    if (!h)
        return 2;

    uint64_t off = 0;
    char err[256];

    /* ── 1. realloc：不存在则创建 ─────────────────────────────── */
    memset(err, 0, sizeof err);
    CHECK(kvspaceRealloc(h, "/v", 8, 5, &off, err, sizeof err) == KVSPACE_OK,
          "realloc create: %s", err);
    CHECK(off != 0, "realloc 返回 0 偏移（契约禁止）");
    uint64_t created = off;

    /* resolve 能定位到同一个偏移。 */
    uint64_t got = 0;
    CHECK(kvspaceResolve(h, "/v", 0 /* anchor 0 = root */, &got, err, sizeof err) ==
              KVSPACE_OK,
          "resolve created: %s", err);
    CHECK(got == created, "resolve 偏移 %llu != realloc %llu",
          (unsigned long long)got, (unsigned long long)created);

    /* 不存在的 key 报 MISSING。 */
    CHECK(kvspaceResolve(h, "/nope", 0 /* anchor 0 = root */, &got, err,
                         sizeof err) == KVSPACE_MISSING,
          "resolve missing key");

    /* ── 2. realloc：容量内原地 ───────────────────────────────── */
    CHECK(kvspaceRealloc(h, "/v", 4, 5, &off, err, sizeof err) == KVSPACE_OK,
          "realloc shrink: %s", err);
    CHECK(off == created, "容量内原地必须返回同一偏移（%llu != %llu）",
          (unsigned long long)off, (unsigned long long)created);
    /* 同容量再写一次仍是原地。 */
    CHECK(kvspaceRealloc(h, "/v", 4, 5, &off, err, sizeof err) == KVSPACE_OK &&
              off == created,
          "realloc 同尺寸仍应原地");

    /* ── 3. realloc：超容量搬移 ───────────────────────────────── */
    CHECK(kvspaceRealloc(h, "/v", 65536, 5, &off, err, sizeof err) == KVSPACE_OK,
          "realloc grow: %s", err);
    CHECK(off != created, "超容量必须搬移（偏移没变：%llu）",
          (unsigned long long)off);
    uint64_t moved = off;
    CHECK(kvspaceResolve(h, "/v", 0 /* anchor 0 = root */, &got, err, sizeof err) ==
              KVSPACE_OK && got == moved,
          "resolve 应指向新偏移");

    /* 搬移后回落：新盒子容量够，故原地。 */
    CHECK(kvspaceRealloc(h, "/v", 8, 5, &off, err, sizeof err) == KVSPACE_OK &&
              off == moved,
          "搬移后缩小应在新盒子内原地");

    /* ── 4. free ──────────────────────────────────────────────── */
    CHECK(kvspaceFree(h, "/v", err, sizeof err) == KVSPACE_OK, "free value");
    CHECK(kvspaceResolve(h, "/v", 0 /* anchor 0 = root */, &got, err, sizeof err) ==
              KVSPACE_MISSING,
          "free 后应 MISSING");
    CHECK(kvspaceFree(h, "/v", err, sizeof err) == KVSPACE_MISSING,
          "重复 free 应 MISSING");

    /* ── 5. makeindex：key 形状校验 + 幂等 ─────────────────────── */
    CHECK(kvspaceMkindex(h, "/d/", err, sizeof err) == KVSPACE_OK,
          "makeindex /d/: %s", err);
    /* 幂等：第二次仍 OK，且不重建（偏移不变）。 */
    uint64_t d_off = 0;
    CHECK(kvspaceResolve(h, "/d/", 0 /* anchor 0 = root */, &d_off, err,
                         sizeof err) == KVSPACE_OK,
          "resolve /d/");
    CHECK(kvspaceMkindex(h, "/d/", err, sizeof err) == KVSPACE_OK,
          "makeindex 幂等");
    CHECK(kvspaceResolve(h, "/d/", 0 /* anchor 0 = root */, &got, err, sizeof err) ==
              KVSPACE_OK && got == d_off,
          "幂等 makeindex 不得搬移索引自身");
    /* 非索引 key 拒绝。 */
    CHECK(kvspaceMkindex(h, "/notindex", err, sizeof err) == KVSPACE_INVALID,
          "makeindex 非索引 key 应 INVALID");

    /* ── 5b. 基址入口：base + offset 指到值；扩容前后 base 不变 ── */
    {
        uint8_t *base = kvspaceXvalueInProcessAddress(h);
        CHECK(base != NULL, "shm 必须有值区基址");

        uint64_t voff = 0;
        CHECK(kvspaceRealloc(h, "/base/probe", 8, 0, &voff, err, sizeof err) ==
                  KVSPACE_OK,
              "realloc probe: %s", err);
        uint64_t roff = 0;
        CHECK(kvspaceResolve(h, "/base/probe", 0, &roff, err, sizeof err) ==
                  KVSPACE_OK,
              "resolve probe: %s", err);
        CHECK(roff == voff, "resolve 偏移应与 realloc 一致");
        /* head 在 base + offset 处：bodylen=8、attrslen=0、padding 全零，
         * 后面紧跟 8 字节 body（realloc 已清零）。 */
        const uint8_t *wire = base + roff;
        uint64_t bodylen = 0;
        for (int i = 0; i < 8; i++)
            bodylen |= (uint64_t)wire[i] << (8 * i);
        CHECK(bodylen == 8, "head bodylen 应为 8（实际 %llu）",
              (unsigned long long)bodylen);
        CHECK(wire[8] == 0 && wire[9] == 0, "attrslen 应为 0");
        {
            int zero = 1;
            for (int i = 10; i < 16; i++)
                if (wire[i] != 0)
                    zero = 0;
            CHECK(zero, "padding 必须全零");
        }
        CHECK(wire[16] == 0 && wire[23] == 0, "新建 body 必须清零");

        /* 基址在扩容前后不变（预留 VA + MAP_FIXED）。用小 data_size 打开
         * 一个新句柄，逼它长大后再比基址。 */
        uint8_t *big = NULL;
        CHECK(kvspaceRealloc(h, "/base/grow", 1u << 20, 0, &big, err,
                             sizeof err) == KVSPACE_OK,
              "realloc 大块触发扩容: %s", err);
        CHECK(kvspaceXvalueInProcessAddress(h) == base, "扩容后基址必须不变");
    }

    /* ── 6. listlen：非索引 / 空索引 / N-M ─────────────────────── */
    int64_t n = -1;
    uint32_t m = 0;
    /* 非索引判据只对**存在**的 key 成立：不存在的 key 是 MISSING（见 9d）。
     * 所以先建出来，再断言 NOT_INDEX。 */
    CHECK(kvspaceRealloc(h, "/notindex", 1, 0, &off, err, sizeof err) ==
              KVSPACE_OK,
          "seed /notindex");
    CHECK(kvspaceListlen(h, "/notindex", &n, &m, err, sizeof err) ==
              KVSPACE_NOT_INDEX,
          "listlen 存在的非索引应 NOT_INDEX");
    CHECK(kvspaceListlen(h, "/d/", &n, &m, err, sizeof err) == KVSPACE_OK,
          "listlen 空索引");
    CHECK(n == 0 && m == 0, "空索引应 N=M=0（实际 N=%lld M=%u）",
          (long long)n, (unsigned)m);

    /* 挂三个子项：两个普通值 + 一个子索引（名字最短但带尾 `/`）。 */
    CHECK(kvspaceRealloc(h, "/d/aa", 2, 0, &off, err, sizeof err) == KVSPACE_OK,
          "realloc /d/aa");
    CHECK(kvspaceRealloc(h, "/d/bbbbbb", 2, 0, &off, err, sizeof err) ==
              KVSPACE_OK,
          "realloc /d/bbbbbb");
    CHECK(kvspaceMkindex(h, "/d/c/", err, sizeof err) == KVSPACE_OK,
          "makeindex /d/c/");

    CHECK(kvspaceListlen(h, "/d/", &n, &m, err, sizeof err) == KVSPACE_OK,
          "listlen 三子项");
    CHECK(n == 3, "N 应为 3（实际 %lld）", (long long)n);
    /* 最长名字是 "bbbbbb"（6 字节）；索引项 "c/" 只有 2 字节。 */
    CHECK(m == 6, "M 应为 6（实际 %u）", (unsigned)m);

    /* ── 7. list：探测 / 容量不足 / 正常 ───────────────────────── */
    uint32_t len = 0, cnt = 0;
    uint64_t gen = 0;
    CHECK(kvspaceList(h, "/d/", NULL, 0, &len, &cnt, &gen, err, sizeof err) ==
              KVSPACE_OK,
          "list NULL 探测: %s", err);
    /* 3 个名字 + 3 个 NUL = 2+1 + 6+1 + 2+1 = 13。 */
    CHECK(len == 13, "探测长度应为 13（实际 %u）", (unsigned)len);
    CHECK(cnt == 3, "探测个数应为 3（实际 %u）", (unsigned)cnt);
    CHECK(gen != 0, "generation 应前进（实际 0）");
    uint64_t gen_before = gen;

    /* 容量不足：CAPACITY，且仍写出所需长度与个数，不截断。 */
    char small[8];
    len = 0;
    cnt = 0;
    gen = 0;
    CHECK(kvspaceList(h, "/d/", small, sizeof small, &len, &cnt, &gen, err,
                      sizeof err) == KVSPACE_CAPACITY,
          "容量不足应 CAPACITY");
    CHECK(len == 13 && cnt == 3, "容量不足仍须写出所需值（len=%u cnt=%u）",
          (unsigned)len, (unsigned)cnt);

    /* 正常：一次复制全部，NUL 分隔，目录项带尾 `/`。 */
    char buf[64];
    len = 0;
    cnt = 0;
    memset(buf, 0x7f, sizeof buf);
    CHECK(kvspaceList(h, "/d/", buf, sizeof buf, &len, &cnt, &gen, err,
                      sizeof err) == KVSPACE_OK,
          "list 正常: %s", err);
    CHECK(len == 13 && cnt == 3, "正常路径 len/cnt");
    {
        /* 名字顺序未定义：逐条取出后按集合比对（不看顺序）。 */
        int seen_aa = 0, seen_bb = 0, seen_c = 0;
        uint32_t p = 0;
        while (p < len) {
            const char *name = buf + p;
            size_t l = strlen(name);
            if (strcmp(name, "aa") == 0)
                seen_aa = 1;
            else if (strcmp(name, "bbbbbb") == 0)
                seen_bb = 1;
            else if (strcmp(name, "c/") == 0)
                seen_c = 1;
            p += (uint32_t)l + 1;
        }
        CHECK(seen_aa && seen_bb && seen_c,
              "list 缺少子项（aa=%d bb=%d c/= %d）", seen_aa, seen_bb, seen_c);
    }

    /* ── 8. generation：值修改不前进，key 集合变化才前进 ───────── */
    CHECK(kvspaceRealloc(h, "/d/aa", 2, 0, &off, err, sizeof err) == KVSPACE_OK,
          "realloc 覆盖已有值");
    gen = 0;
    CHECK(kvspaceList(h, "/d/", NULL, 0, &len, &cnt, &gen, err, sizeof err) ==
              KVSPACE_OK,
          "list 取值后 generation");
    CHECK(gen == gen_before,
          "纯值修改不得前进 generation（%llu -> %llu）",
          (unsigned long long)gen_before, (unsigned long long)gen);

    CHECK(kvspaceRealloc(h, "/d/cc", 2, 0, &off, err, sizeof err) == KVSPACE_OK,
          "realloc 新增 key");
    gen = 0;
    CHECK(kvspaceList(h, "/d/", NULL, 0, &len, &cnt, &gen, err, sizeof err) ==
              KVSPACE_OK,
          "list 新增后 generation");
    CHECK(gen != gen_before, "新增 key 必须前进 generation");
    uint64_t gen_after_add = gen;

    CHECK(kvspaceFree(h, "/d/cc", err, sizeof err) == KVSPACE_OK,
          "free /d/cc");
    gen = 0;
    CHECK(kvspaceList(h, "/d/", NULL, 0, &len, &cnt, &gen, err, sizeof err) ==
              KVSPACE_OK,
          "list 删除后 generation");
    CHECK(gen != gen_after_add, "删除 key 必须前进 generation");

    /* ── 9. free 非空索引 → HAS_CHILDREN；清空后可删 ───────────── */
    CHECK(kvspaceFree(h, "/d/", err, sizeof err) == KVSPACE_HAS_CHILDREN,
          "非空索引应 HAS_CHILDREN（实际 rc 见 err: %s）", err);
    CHECK(kvspaceFree(h, "/d/aa", err, sizeof err) == KVSPACE_OK, "free aa");
    CHECK(kvspaceFree(h, "/d/bbbbbb", err, sizeof err) == KVSPACE_OK, "free bb");
    /* 子索引 c/ 空着，可删。 */
    CHECK(kvspaceFree(h, "/d/c/", err, sizeof err) == KVSPACE_OK, "free c/");
    CHECK(kvspaceFree(h, "/d/", err, sizeof err) == KVSPACE_OK,
          "空索引应可删（实际：%s）", err);
    CHECK(kvspaceResolve(h, "/d/", 0 /* anchor 0 = root */, &got, err, sizeof err) ==
              KVSPACE_MISSING,
          "空索引删后应 MISSING");

    /* ── 9b. deltree：根与后代都 MISSING，兄弟不受影响，幂等 ───── */
    {
        uint64_t x = 0;
        CHECK(kvspaceMkindex(h, "/dt/", err, sizeof err) == KVSPACE_OK,
              "deltree seed /dt/");
        CHECK(kvspaceMkindex(h, "/dt/src/", err, sizeof err) == KVSPACE_OK,
              "deltree seed /dt/src/");
        /* 同名的值形态与目录形态并存：两种都必须被删掉。 */
        CHECK(kvspaceRealloc(h, "/dt/src", 2, 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "deltree seed /dt/src value form");
        CHECK(kvspaceRealloc(h, "/dt/src/a", 1, 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "deltree seed /dt/src/a");
        CHECK(kvspaceMkindex(h, "/dt/src/sub/", err, sizeof err) == KVSPACE_OK,
              "deltree seed /dt/src/sub/");
        CHECK(kvspaceRealloc(h, "/dt/src/sub/y", 1, 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "deltree seed /dt/src/sub/y");
        CHECK(kvspaceRealloc(h, "/dt/keep", 1, 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "deltree seed /dt/keep");
        CHECK(kvspaceRealloc(h, "/dt/keepx/a", 1, 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "deltree seed /dt/keepx/a");

        CHECK(kvspaceDeltree(h, "/dt/src/", err, sizeof err) == KVSPACE_OK,
              "deltree: %s", err);
        /* 根：带尾 `/` 与不带尾的两种形态都必须 MISSING。 */
        CHECK(kvspaceResolve(h, "/dt/src", 0, &x, err, sizeof err) ==
                  KVSPACE_MISSING,
              "deltree 后根（值形态）应 MISSING");
        CHECK(kvspaceResolve(h, "/dt/src/", 0, &x, err, sizeof err) ==
                  KVSPACE_MISSING,
              "deltree 后根（目录形态）应 MISSING");
        CHECK(kvspaceResolve(h, "/dt/src/a", 0, &x, err, sizeof err) ==
                  KVSPACE_MISSING,
              "deltree 后后代 a 应 MISSING");
        CHECK(kvspaceResolve(h, "/dt/src/sub/y", 0, &x, err, sizeof err) ==
                  KVSPACE_MISSING,
              "deltree 后后代 sub/y 应 MISSING");
        /* 兄弟不受影响：前缀相同但是不同子树。 */
        CHECK(kvspaceResolve(h, "/dt/keep", 0, &x, err, sizeof err) == KVSPACE_OK,
              "deltree 不得误删兄弟 /dt/keep");
        CHECK(kvspaceResolve(h, "/dt/keepx/a", 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "deltree 不得误删 /dt/keepx/a");
        /* 幂等：再删一次仍 OK，不存在的 key 也 OK。 */
        CHECK(kvspaceDeltree(h, "/dt/src/", err, sizeof err) == KVSPACE_OK,
              "deltree 应幂等");
        CHECK(kvspaceDeltree(h, "/dt/nope/", err, sizeof err) == KVSPACE_OK,
              "deltree 不存在的 key 应幂等成功");
        CHECK(kvspaceResolve(h, "/dt/keep", 0, &x, err, sizeof err) == KVSPACE_OK,
              "幂等 deltree 不得波及兄弟");
    }

    /* ── 9c. listlen/list：索引不存在 ≠ 空索引 ───────────────────
     * 契约：key 不存在必须 MISSING；只有 key 存在且无成员才是 OK/0/0。
     * 这条不能靠「扫出 0 条」推断——删值后 ART 节点还在（墓碑），
     * 扫出 0 条既可能是空目录，也可能是整个 key 都没了。 */
    {
        uint64_t x = 0;
        CHECK(kvspaceMkindex(h, "/gone/", err, sizeof err) == KVSPACE_OK,
              "seed /gone/");
        CHECK(kvspaceFree(h, "/gone/", err, sizeof err) == KVSPACE_OK,
              "free /gone/");
        int64_t gc = -1;
        uint32_t gm = 99;
        CHECK(kvspaceListlen(h, "/gone/", &gc, &gm, err, sizeof err) ==
                  KVSPACE_MISSING,
              "不存在的索引 listlen 必须 MISSING（实际 n=%lld）", (long long)gc);
        uint32_t glen = 77, gcnt = 77;
        uint64_t ggen = 0;
        CHECK(kvspaceList(h, "/gone/", NULL, 0, &glen, &gcnt, &ggen, err,
                          sizeof err) == KVSPACE_MISSING,
              "不存在的索引 list 必须 MISSING");

        /* 反向对照：存在的空索引必须是 OK/0/0，不能也报 MISSING。 */
        CHECK(kvspaceMkindex(h, "/empty/", err, sizeof err) == KVSPACE_OK,
              "seed /empty/");
        CHECK(kvspaceListlen(h, "/empty/", &gc, &gm, err, sizeof err) ==
                  KVSPACE_OK && gc == 0 && gm == 0,
              "存在的空索引必须 OK/0/0（实际的 n=%lld m=%u）",
              (long long)gc, (unsigned)gm);
        CHECK(kvspaceList(h, "/empty/", NULL, 0, &glen, &gcnt, &ggen, err,
                          sizeof err) == KVSPACE_OK && glen == 0 && gcnt == 0,
              "存在的空索引 list 必须 OK/0/0");
    }

    /* ── 9d. 三态判据的顺序：先存在性，后形态 ───────────────────
     * 1 不存在（带尾分隔符或不带）⇒ MISSING
     * 2 存在但非索引形态         ⇒ NOT_INDEX
     * 3 存在且是空索引           ⇒ OK/0/0
     * 顺序不可交换：不存在的普通 key 若先判形态会得到 NOT_INDEX，
     * 调用方会以为「key 存在但用错了接口」，从而掩盖真正的缺失。 */
    {
        uint64_t x = 0;
        int64_t tc = -1;
        uint32_t tm = 99;
        uint32_t tl = 99, tn = 99;
        uint64_t tg = 0;

        /* 1：不存在的 key——带尾 `/` 与不带尾都是 MISSING。 */
        CHECK(kvspaceListlen(h, "/never", &tc, &tm, err, sizeof err) ==
                  KVSPACE_MISSING,
              "不存在的普通 key 应 MISSING（不是 NOT_INDEX）");
        CHECK(kvspaceListlen(h, "/never/", &tc, &tm, err, sizeof err) ==
                  KVSPACE_MISSING,
              "不存在的索引形态 key 应 MISSING");
        CHECK(kvspaceList(h, "/never", NULL, 0, &tl, &tn, &tg, err,
                          sizeof err) == KVSPACE_MISSING,
              "list 不存在的普通 key 应 MISSING");

        /* 2：存在的普通值（非索引形态）——NOT_INDEX。 */
        CHECK(kvspaceRealloc(h, "/plain", 1, 0, &x, err, sizeof err) ==
                  KVSPACE_OK,
              "seed /plain");
        CHECK(kvspaceListlen(h, "/plain", &tc, &tm, err, sizeof err) ==
                  KVSPACE_NOT_INDEX,
              "存在的非索引 key 应 NOT_INDEX");
        CHECK(kvspaceList(h, "/plain", NULL, 0, &tl, &tn, &tg, err,
                          sizeof err) == KVSPACE_NOT_INDEX,
              "list 存在的非索引 key 应 NOT_INDEX");

        /* 3：存在的空索引——OK/0/0（已由 9c 覆盖，这里只做同批三态对照）。 */
        CHECK(kvspaceListlen(h, "/empty/", &tc, &tm, err, sizeof err) ==
                  KVSPACE_OK && tc == 0 && tm == 0,
              "同批对照：存在的空索引仍应 OK/0/0");
    }

    /* ── 9e. 索引只靠后代也算存在（无根值） ─────────────────────
     * `/only/` 自己没有根值，只有 `/only/child/`。目录身份由 key 形状决定，
     * 不靠根值：有后代 ⇒ 这个目录存在 ⇒ listlen 返回 OK n=1。
     * 而 `/only`（不带尾 `/`）问的是「有没有一个值」，没有就是 NOT_INDEX
     * ——两者是**不同的 key 形态**，不能互相顶替。
     * `resolve("/only/")` 问的是值，仍可 MISSING：这正是 resolve 与 listlen
     * 语义不同的地方。 */
    {
        uint64_t y = 0;
        int64_t oc = -1;
        uint32_t om = 0;

        CHECK(kvspaceMkindex(h, "/only/child/", err, sizeof err) == KVSPACE_OK,
              "seed /only/child/");
        /* 索引形态：有后代即存在，且后代数就是 1。 */
        CHECK(kvspaceListlen(h, "/only/", &oc, &om, err, sizeof err) ==
                  KVSPACE_OK && oc == 1,
              "只有后代的索引 listlen 必须 OK n=1（实际 rc/n 见上）");
        CHECK(om == 6, "最长子项名应是 \"child/\"（6 字节），实际 %u",
              (unsigned)om);
        /* resolve 问值：该索引没有根值，MISSING 是对的。 */
        CHECK(kvspaceResolve(h, "/only/", 0, &y, err, sizeof err) ==
                  KVSPACE_MISSING,
              "无根值的索引 resolve 应 MISSING（问值，不问子项）");
        /* 值形态：/only 与 /only/ 是两个 key，前者没有值也没有同名目录值。 */
        CHECK(kvspaceListlen(h, "/only", &oc, &om, err, sizeof err) ==
                  KVSPACE_MISSING,
              "不存在的值形态 /only 应 MISSING");
        /* 反向对照：给 /only/ 补根值后，值形态 /only 也该被认作存在。 */
        CHECK(kvspaceRealloc(h, "/only/", 1, 0, &y, err, sizeof err) ==
                  KVSPACE_OK,
              "给 /only/ 补根值");
        CHECK(kvspaceListlen(h, "/only", &oc, &om, err, sizeof err) ==
                  KVSPACE_NOT_INDEX,
              "同名的值形态 /only 在同名目录有根值后应 NOT_INDEX");
    }

    /* ── 10. resolve：失效 anchor ─────────────────────────────── */
    /* 公共 anchor 是 key 区 int64 字节偏移。远超 ART 区大小 → 必失效。 */
    const int64_t dead_anchor = INT64_C(1) << 40;
    CHECK(kvspaceResolve(h, "/x", dead_anchor, &got, err, sizeof err) ==
              KVSPACE_ANCHOR_INVALID,
          "越界 key 区偏移应 ANCHOR_INVALID");
    /* 非块数据偏移（未对齐）同样必须拒绝。 */
    CHECK(kvspaceResolve(h, "/x", 1, &got, err, sizeof err) ==
              KVSPACE_ANCHOR_INVALID,
          "错位偏移应 ANCHOR_INVALID");

    kvspaceClose(h);
    char cmd[320];
    snprintf(cmd, sizeof cmd, "rm -rf %s", dir);
    if (system(cmd) != 0)
        fprintf(stderr, "cleanup failed\n");

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("ok: storage abi\n");
    return 0;
}
