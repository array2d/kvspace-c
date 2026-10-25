#ifndef XVALUE_META_H
#define XVALUE_META_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int kvspaceMetaKey(const char *key, char **out);
/* 栈上构造 meta key；buf 至少 17 + 2*strlen(key)。 */
int kvspaceMetaKeyInto(const char *key, char *buf, size_t cap);
int kvspaceMetaEncode(uint8_t ro, uint32_t vid, uint8_t **out, uint64_t *out_len);
int kvspaceMetaDecode(const uint8_t *data, uint64_t len, uint8_t *ro, uint32_t *vid);

#ifdef __cplusplus
}
#endif

#endif
