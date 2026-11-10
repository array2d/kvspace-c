#define _GNU_SOURCE
#include "kvspace_shm.h"
#include <kvspace/kvlang_xvalue.h>
#include "xvalue_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

/* 固定 head 16 字节 [bodylen:u64][attrslen:u16][padding:6]；body 偏移恒 16，
   attr 区紧随 body 且**没有**长度前缀。 */

/* 新 ABI 写路径：Realloc 给容量与 offset，body 由调用方在基址上直接填。 */
static int write_new_body(kvspace_t *kv, const char *key, uint64_t body_len,
                          uint8_t **body) {
    uint64_t offset = 0;
    if (kvspaceShmStorageRealloc(kv, key, body_len, 0, &offset) != 0)
        return -1;
    *body = kvspaceShmStorageDataBase(kv) + offset + KVSPACE_XVALUE_HEAD_LEN;
    return 0;
}

static int wire_len_of(const uint8_t *stored) {
    kvspaceXvalueHead h;
    memcpy(&h, stored, sizeof h);
    return (int)kvspaceXvalueHeadTotalLen(&h);
}

int main(void) {
    char dir[] = "/tmp/kvspace-new-wire-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/db", dir);
    kvspace_t *kv = kvspaceShmOpen(path, 8UL * 64 * 64 * 64 * 64);
    CHECK(kv != NULL);
    uint8_t *value = NULL;
    uint64_t n = 0;
    uint8_t raw[8] = {42};
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "",
                                     &value, &n) == 0);
    CHECK(n == 16 + 8);
    CHECK(kvspaceShmSet(kv, "/x", value, (int32_t)n) == 0);
    free(value);
    int32_t got = 0;
    uint8_t *stored = kvspaceShmGet(kv, "/x", 0, &got);
    CHECK(stored && got == 16 + 8 && stored[KVSPACE_XVALUE_HEAD_LEN] == 42);

    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, "", &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/p", value, (int32_t)n) == 0);
    free(value);
    stored = kvspaceShmGet(kv, "/p", 1, &got);
    CHECK(stored && got == 16 + 8 && stored[KVSPACE_XVALUE_HEAD_LEN] == 42);

    CHECK(kvspaceXvalueHeadNewNone(&value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/m", value, (int32_t)n) == 0);
    free(value);
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "",
                                     &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/m·[0]", value, (int32_t)n) == 0);
    free(value);
    char **names = NULL;
    int32_t count = 0;
    CHECK(kvspaceShmList(kv, "/m·", false, 0, &names, &count) == 0 &&
          count == 1 && strcmp(names[0], "[0]") == 0);
    free(names[0]);
    free(names);
    CHECK(kvspaceShmCptree(kv, "/m", "/m-tree") == 0);
    CHECK(kvspaceShmCplist(kv, "/m", "/m-list") == 0);
    CHECK(kvspaceShmCptree(kv, "/m", "/m") == 0);
    CHECK(kvspaceShmCplist(kv, "/m", "/m") == 0);
    CHECK(kvspaceShmCptree(kv, "/m", "/m/child") != 0);
    CHECK(kvspaceShmCptree(kv, "/", "/under-root") != 0);
    for (int i = 0; i < 2; i++) {
        const char *base = i ? "/m-list" : "/m-tree";
        char member[32];
        snprintf(member, sizeof member, "%s·[0]", base);
        stored = kvspaceShmGet(kv, member, 0, &got);
        CHECK(stored && stored[KVSPACE_XVALUE_HEAD_LEN] == 42);
    }
    CHECK(kvspaceShmGet(kv, "/m-tree·", 0, &got) == NULL);
    CHECK(kvspaceShmGet(kv, "/m-list·", 0, &got) == NULL);

    CHECK(kvspaceXvalueHeadNewNone(&value, &n) == 0);
    CHECK(kvspaceShmSetValue(kv, "/fn/", value, (int32_t)n) == 0);
    free(value);
    CHECK(kvspaceShmMkindex(kv, "/fn/", 0) == 0);

    CHECK(kvspaceShmMkindex(kv, "/lib/f/", 0) == 0);
    const uint8_t code[] = {0, 0, 0, 0, 0, 'x'};
    CHECK(kvspaceXvalueHeadNewCode(code, sizeof code, "", &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/lib/f/[1,-1]", value, (int32_t)n) == 0);
    CHECK(kvspaceShmSet(kv, "/lib/f/[1,0]", value, (int32_t)n) == 0);
    free(value);
    CHECK(kvspaceShmExtindex(kv, "/vthread/1/[1]/", "/lib/f/", NULL, 0) == 0);
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, "", &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/vthread/1/[1]/[1,0]", value, (int32_t)n) != 0);
    CHECK(kvspaceShmSetValue(kv, "/vthread/1/[1]/[1,-1]", value,
                             (int32_t)n) == 0);
    free(value);
    stored = kvspaceShmGet(kv, "/vthread/1/[1]/[1,-1]", 0, &got);
    CHECK(stored != NULL);
    /* 引用性由 langtype 的 `*` 前缀表达，不在 head——判据直接对**读回来的
       这条 wire**取，不是对上一条。 */
    CHECK(kvspace_c_reference_of(stored, (uint64_t)got) ==
          KVSPACE_XVALUE_REFERENCE_PTR);

    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "",
                                     &value, &n) == 0);
    CHECK(kvspaceShmSetValue(kv, "/vthread/1/[1]/[1,-1]", value,
                             (int32_t)n) == 0);
    CHECK(kvspaceShmSet(kv, "/vthread/1/[1]/[1,-1]", value, (int32_t)n) == 0);
    CHECK(kvspaceShmSet(kv, "/vthread/1/[1]/[1,-1]/child", value,
                        (int32_t)n) == 0);
    CHECK(kvspaceShmDel(kv, "/vthread/1/[1]/[1,-1]") == 0);
    CHECK(kvspaceShmSet(kv, "/vthread/1/[1]/[1,-1]", value, (int32_t)n) != 0);
    free(value);

    uint8_t *body = NULL;
    CHECK(write_new_body(kv, "/x", 8, &body) == 0);
    body[0] = 43;
    stored = kvspaceShmGet(kv, "/x", 0, &got);
    CHECK(stored && stored[KVSPACE_XVALUE_HEAD_LEN] == 43);

    CHECK(write_new_body(kv, "/fresh", 8, &body) == 0);
    memcpy(body, raw, 8);
    stored = kvspaceShmGet(kv, "/fresh", 0, &got);
    CHECK(stored && stored[KVSPACE_XVALUE_HEAD_LEN] == 42);
    CHECK(kvspaceShmCp(kv, "/fresh", "/fresh-copy") == 0);
    CHECK(kvspaceShmCptree(kv, "/fresh", "/fresh-tree") == 0);

    static const uint8_t utf8[] = {0xc3, 0xa9, 'a'};
    CHECK(write_new_body(kv, "/text", 3, &body) == 0);
    memcpy(body, utf8, sizeof utf8);
    stored = kvspaceShmGet(kv, "/text", 0, &got);
    CHECK(stored && wire_len_of(stored) == (int)(KVSPACE_XVALUE_HEAD_LEN + 3) &&
          memcmp(stored + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    body[0] = 0xff;
    /* 固定头存储层不校验 UTF-8（码点校验移明文层按 attr 类型做）——可读回。 */
    CHECK(kvspaceShmGet(kv, "/text", 0, &got) != NULL);
    body[0] = utf8[0];
    CHECK(kvspaceShmGet(kv, "/text", 1, &got) != NULL);

    uint8_t boolean = 1;
    CHECK(kvspaceXvalueHeadNewScalar(&boolean, 1, "",
                                     &value, &n) == 0);
    CHECK(kvspaceShmSet(kv, "/flag", value, (int32_t)n) == 0);
    free(value);
    stored = kvspaceShmGet(kv, "/flag", 0, &got);
    CHECK(stored != NULL);
    uint8_t *flag_body = stored + KVSPACE_XVALUE_HEAD_LEN;
    flag_body[0] = 2;
    /* 存储层不校验 bool 值域（明文层按 attr key 类型校验）——可读回。 */
    CHECK(kvspaceShmGet(kv, "/flag", 0, &got) != NULL);
    flag_body[0] = 1;
    CHECK(kvspaceShmGet(kv, "/flag", 1, &got) != NULL);
    kvspaceShmClose(kv);
    unlink(path);
    char extra[144];
    snprintf(extra, sizeof extra, "rm -rf %s", dir);
    CHECK(system(extra) == 0);
    puts("ok");
    return 0;
}
