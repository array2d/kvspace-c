#include <kvspace/kvspace.h>
#include "xvalue_head.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

int main(void) {
    uint8_t *value = NULL;
    uint32_t len = 0;
    kvspaceHead_t head;
    CHECK(kvspaceXvalueEncode("None", NULL, 0, NULL, 0, &value, &len) == 0);
    CHECK(len == 32 && kvspaceDecodeHead(value, len, &head) == 0 &&
          head.langtype_len == 0 && head.body_len == 0);
    free(value);

    CHECK(kvspaceNewInt64(42, &value, &len) == 0);
    CHECK(len == 40 && value[0] == 5 && value[1] == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          strcmp(head.langtype, "int64") == 0 && head.body_len == 8);
    free(value);

    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceNewChar(utf8, sizeof utf8, &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          strcmp(head.langtype, "[2]char/utf8") == 0 &&
          head.body_len == (int32_t)sizeof utf8 && head.body_cap == sizeof utf8 &&
          head.body_offset == 64);
    free(value);

    const int32_t dims[2] = {2, 3};
    uint8_t raw[24] = {0};
    CHECK(kvspaceXvalueEncode("float32", raw, sizeof raw - 1, dims, 2,
                           &value, &len) != 0);
    CHECK(kvspaceXvalueEncode("float32", raw, sizeof raw, dims, 2,
                           &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.ndim == 2 && head.dims[0] == 2 && head.dims[1] == 3 &&
          head.body_offset == 128);
    free(value);

    CHECK(kvspaceNewPtr("int64", "/x", &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.ref == KVSPACE_REF_PTR && head.body_len == 2);
    free(value);
    /* 空 kindexpr = 不约束：Ptr 目标允许为空；`any` 不是类型名，创建即拒。 */
    CHECK(kvspaceNewPtr("", "/x", &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.ref == KVSPACE_REF_PTR && head.langtype_len == 0 &&
          head.body_len == 2);
    free(value);
    CHECK(kvspaceNewPtr("any", "/x", &value, &len) != 0);
    CHECK(kvspaceXvalueEncodeMode("int64", (const uint8_t *)"s3://", 5,
                                NULL, 0, KVSPACE_REF_EXT, 0, 0,
                                &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.ref == KVSPACE_REF_EXT && head.storetype == KVSPACE_XH_EXT);
    free(value);
    CHECK(kvspaceXvalueEncodeMode("int64", raw, 8, NULL, 0, 0, 1, 0,
                                &value, &len) != 0);

    CHECK(kvspaceXvalueEncode("rwfunc", NULL, 0, NULL, 0, &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.storetype == KVSPACE_XH_FIXED_SMALL && head.body_len == 0);
    free(value);
    uint8_t anchor[5] = {0};
    CHECK(kvspaceXvalueEncode("rwfunc", anchor, 5, NULL, 0, &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.storetype == KVSPACE_XH_SLACK && head.body_len == 5);
    free(value);
    CHECK(kvspaceXvalueEncode("[int64]·[]char/utf32", NULL, 0, NULL, 0,
                           &value, &len) == 0);
    CHECK(kvspaceDecodeHead(value, len, &head) == 0 &&
          head.storetype == KVSPACE_XH_FIXED_SMALL && head.ndim == 0);
    free(value);
    puts("ok");
    return 0;
}
