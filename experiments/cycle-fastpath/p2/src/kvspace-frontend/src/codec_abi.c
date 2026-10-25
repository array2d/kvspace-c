#include "kvspace/kvspace.h"
#include "xvalue_head.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int finish(uint8_t *value, uint64_t len, uint8_t **out, uint32_t *out_len) {
    if (!value || len > UINT32_MAX) {
        free(value);
        return 1;
    }
    *out = value;
    *out_len = (uint32_t)len;
    return 0;
}

static int locator(int ext, const char *type, const uint8_t *raw, uint32_t len,
                   uint8_t **out, uint32_t *out_len) {
    if (!raw || memchr(raw, 0, len))
        return 1;
    char *path = malloc((size_t)len + 1);
    if (!path)
        return 1;
    memcpy(path, raw, len);
    path[len] = 0;
    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = ext ? kvspaceXhNewExt(type, path, len, &value, &total) :
                   kvspaceXhNewPtr(type, path, len, &value, &total);
    free(path);
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

int kvspaceXvalueEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                        const int32_t *dims, int32_t ndim, uint8_t **out,
                        uint32_t *out_len) {
    return kvspaceXvalueEncodeMode(kind, raw, raw_len, dims, ndim, 0, 0, 0, out, out_len);
}

int kvspaceXvalueEncodeMode(const char *kind, const uint8_t *raw,
                            uint32_t raw_len, const int32_t *dims, int32_t ndim,
                            int32_t ref, uint8_t ro, uint32_t vid, uint8_t **out,
                            uint32_t *out_len) {
    if (!out || !out_len || !kind || ndim < 0 || ndim > 8 ||
        (ndim && !dims) || (raw_len && !raw) || ro || vid)
        return 1;
    *out = NULL;
    *out_len = 0;
    if (ref == KVSPACE_REF_PTR || ref == KVSPACE_REF_EXT)
        return locator(ref == KVSPACE_REF_EXT, kind, raw, raw_len, out, out_len);
    if (ref != KVSPACE_REF_INLINE)
        return 1;

    uint8_t *value = NULL;
    uint64_t total = 0;
    int rc = -1;
    if ((kind[0] == 0 || strcmp(kind, "None") == 0) && raw_len == 0 && ndim == 0) {
        rc = kvspaceXhNewNone(&value, &total);
    } else if (strcmp(kind, KVSPACE_KIND_CHAR_UTF8) == 0 ||
               strcmp(kind, KVSPACE_KIND_CHAR_ASCII) == 0 ||
               strcmp(kind, KVSPACE_KIND_CHAR) == 0) {
        int elem = strcmp(kind, KVSPACE_KIND_CHAR_UTF8) == 0 ? KVSPACE_XH_UTF8 :
                   strcmp(kind, KVSPACE_KIND_CHAR_ASCII) == 0 ? KVSPACE_XH_ASCII :
                   KVSPACE_XH_UTF32;
        rc = kvspaceXhNewSlack(elem, raw, raw_len, raw_len, &value, &total);
    } else if (ndim > 0) {
        uint64_t shape[8];
        for (int32_t i = 0; i < ndim; i++) {
            if (dims[i] < 0)
                return 1;
            shape[i] = (uint64_t)dims[i];
        }
        rc = kvspaceXhNewTensor(shape, (uint32_t)ndim, kind, raw, raw_len,
                                &value, &total);
    } else if (strcmp(kind, "rwir") == 0 || strcmp(kind, "def langtype") == 0 ||
               (strcmp(kind, "rwfunc") == 0 && raw_len != 0)) {
        rc = kvspaceXhNewCode(kind, raw, raw_len, raw_len, &value, &total);
    } else if (kvspaceXhNewScalar(kind, raw, raw_len, &value, &total) == 0) {
        rc = 0;
    } else {
        rc = kvspaceXhNewShort(kind, raw, raw_len, &value, &total);
    }
    return rc == 0 ? finish(value, total, out, out_len) : 1;
}

static int parse_dims(const char *type, int32_t *dims, int32_t *ndim) {
    *ndim = 0;
    if (type[0] != '[')
        return 0;
    const char *p = type + 1;
    for (;;) {
        if (*ndim == 8 || *p < '0' || *p > '9')
            return -1;
        uint64_t n = 0;
        do {
            n = n * 10 + (uint64_t)(*p++ - '0');
            if (n > INT32_MAX)
                return -1;
        } while (*p >= '0' && *p <= '9');
        dims[(*ndim)++] = (int32_t)n;
        if (*p == ']')
            return 0;
        if (*p++ != ',')
            return -1;
    }
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out)
        return 1;
    memset(out, 0, sizeof *out);
    kvspaceXh h;
    if (kvspaceXhDecode(data, data_len, &h) != 0 || h.total != data_len ||
        h.headlen > UINT16_MAX || h.content_len > INT32_MAX ||
        h.langtype_len > sizeof out->langtype - 1)
        return 1;
    out->headlen = (uint16_t)h.headlen;
    out->ref = h.kind & KVSPACE_XH_PTR_FLAG ? KVSPACE_REF_PTR :
               h.kind == KVSPACE_XH_EXT ? KVSPACE_REF_EXT : KVSPACE_REF_INLINE;
    out->storetype = h.kind & 3;
    out->body_len = (int32_t)h.content_len;
    out->body_offset = (int32_t)h.headlen;
    out->body_cap = h.body_cap;
    out->langtype_len = (int32_t)h.langtype_len;
    memcpy(out->langtype, h.langtype, h.langtype_len);
    if (h.kind == KVSPACE_XH_FIXED_LARGE ||
        (h.kind == KVSPACE_XH_SLACK && h.langtype_len && h.langtype[0] == '[')) {
        if (parse_dims(out->langtype, out->dims, &out->ndim) != 0)
            return 1;
    }
    return 0;
}

int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len)
        return 1;
    /* `any` 不是类型名（空 kindexpr = 不约束，见 spec [[文法与合法性]]）。 */
    if (strcmp(target_kindexpr, "any") == 0)
        return 1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXhNewPtr(target_kindexpr, target, strlen(target), &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    return kvspaceXvalueEncode(KVSPACE_KIND_CHAR_UTF8, bytes, len, NULL, 0, out, out_len);
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t b = v ? 1 : 0;
    return kvspaceXvalueEncode(KVSPACE_KIND_BOOL, &b, 1, NULL, 0, out, out_len);
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)((uint64_t)v >> (8 * i));
    return kvspaceXvalueEncode(KVSPACE_KIND_INT64, raw, 8, NULL, 0, out, out_len);
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)(bits >> (8 * i));
    return kvspaceXvalueEncode(KVSPACE_KIND_FLOAT64, raw, 8, NULL, 0, out, out_len);
}
