#include "xvalue_meta.h"
#include "xvalue_head.h"

#include <stdlib.h>
#include <string.h>

int kvspaceMetaKeyInto(const char *key, char *buf, size_t cap) {
    static const char prefix[] = "/.kvspace-meta/";
    static const char hex[] = "0123456789abcdef";
    if (!key || !buf || key[0] != '/' ||
        (strncmp(key, prefix, sizeof prefix - 2) == 0 &&
         (key[sizeof prefix - 2] == '/' || key[sizeof prefix - 2] == 0)))
        return -1;
    size_t len = strlen(key);
    if (len > (SIZE_MAX - sizeof prefix) / 2 || sizeof prefix + 2 * len > cap)
        return -1;
    memcpy(buf, prefix, sizeof prefix - 1);
    for (size_t i = 0; i < len; i++) {
        unsigned char b = (unsigned char)key[i];
        buf[sizeof prefix - 1 + 2 * i] = hex[b >> 4];
        buf[sizeof prefix + 2 * i] = hex[b & 15];
    }
    buf[sizeof prefix - 1 + 2 * len] = 0;
    return 0;
}

int kvspaceMetaKey(const char *key, char **out) {
    if (!out)
        return -1;
    *out = NULL;
    if (!key)
        return -1;
    size_t len = strlen(key);
    if (len > (SIZE_MAX - 17) / 2)
        return -1;
    char *p = malloc(17 + 2 * len);
    if (!p)
        return -1;
    if (kvspaceMetaKeyInto(key, p, 17 + 2 * len) != 0) {
        free(p);
        return -1;
    }
    *out = p;
    return 0;
}

int kvspaceMetaEncode(uint8_t ro, uint32_t vid, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    *out_len = 0;
    if (ro > 1)
        return -1;
    uint8_t body[5] = {ro, (uint8_t)vid, (uint8_t)(vid >> 8),
                       (uint8_t)(vid >> 16), (uint8_t)(vid >> 24)};
    return kvspaceXhNewSlack(KVSPACE_XH_BYTE, body, sizeof body, sizeof body,
                             out, out_len);
}

int kvspaceMetaDecode(const uint8_t *data, uint64_t len, uint8_t *ro, uint32_t *vid) {
    if (!ro || !vid)
        return -1;
    kvspaceXh h;
    if (kvspaceXhDecode(data, len, &h) != 0 || h.total != len ||
        h.kind != KVSPACE_XH_SLACK || h.a != 5 || h.b != 5 ||
        h.langtype_len != 8 || memcmp(h.langtype, "[5]uint8", 8) != 0 ||
        h.body[0] > 1)
        return -1;
    *ro = h.body[0];
    *vid = (uint32_t)h.body[1] | (uint32_t)h.body[2] << 8 |
           (uint32_t)h.body[3] << 16 | (uint32_t)h.body[4] << 24;
    return 0;
}
