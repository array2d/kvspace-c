/* test_shm_reopen [dir] — reopen + overwrite churn (#17) */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define DATA_SIZE (8UL * 64 * 64 * 64) /* 8*64^3 = 2MB */
#define NKEYS 900
#define GROW_SIZES 4

static const int32_t k_grow[GROW_SIZES] = {48, 200, 800, 48};

static int failures = 0;
#define CHECK(cond, ...)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);                   \
      fprintf(stderr, __VA_ARGS__);                                            \
      fprintf(stderr, "\n");                                                   \
    }                                                                          \
  } while (0)
#define REQUIRE(cond, ...)                                                     \
  do {                                                                         \
    CHECK(cond, __VA_ARGS__);                                                  \
    if (!(cond))                                                               \
      return;                                                                  \
  } while (0)

static void on_alrm(int s) {
  (void)s;
  fprintf(stderr, "TIMEOUT: sbo_alloc likely spinning (#17)\n");
  _exit(99);
}

static void key_of(char *buf, size_t n, int i) {
  snprintf(buf, n, "/o/%04d", i);
}

static int set_int32(kvspace_t *kv, const char *key, int32_t v) {
  uint8_t raw[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16),
                    (uint8_t)(v >> 24)};
  uint8_t *tlv;
  int32_t n = kvspaceXvalueEncode(KVSPACE_KIND_INT32, raw, 4, NULL, 0, &tlv);
  int rc = kvspaceShmSet(kv, key, tlv, n);
  free(tlv);
  return rc;
}

static int32_t get_int32(kvspace_t *kv, const char *key) {
  int32_t len = 0;
  uint8_t *d = kvspaceShmGet(kv, key, 1, &len);
  if (!d || len <= 0)
    return -1;
  xvalue_head_t h = kvspaceXvalueDecodeHead(d, len);
  return h.raw_len == 4 ? kvspaceXvalueRawInt32(h.raw) : -1;
}

static int set_bytes(kvspace_t *kv, const char *key, int32_t n, int seed) {
  uint8_t *raw = malloc((size_t)n);
  if (!raw)
    return -1;
  for (int32_t j = 0; j < n; j++)
    raw[j] = (uint8_t)(seed + j);
  uint8_t *tlv;
  int32_t dims[1] = {n};
  int32_t len = kvspaceXvalueEncode(KVSPACE_KIND_UINT8, raw, n, dims, 1, &tlv);
  int rc = kvspaceShmSet(kv, key, tlv, len);
  free(tlv);
  free(raw);
  return rc;
}

static int check_bytes(kvspace_t *kv, const char *key, int32_t n, int seed) {
  int32_t len = 0;
  uint8_t *d = kvspaceShmGet(kv, key, 1, &len);
  if (!d || len <= 0)
    return 1;
  xvalue_head_t h = kvspaceXvalueDecodeHead(d, len);
  if (h.raw_len != n)
    return 2;
  for (int32_t j = 0; j < n; j++)
    if (h.raw[j] != (uint8_t)(seed + j))
      return 3;
  return 0;
}

static int wnp_int32(kvspace_t *kv, const char *key, int32_t v) {
  uint8_t *body;
  if (kvspaceShmWriteNewPlace(kv, key, KVSPACE_REF_INLINE,
                              KVSPACE_STORETYPE_ATOM, 0, 0, KVSPACE_KIND_INT32,
                              4, &body) != 0)
    return -1;
  body[0] = (uint8_t)v;
  body[1] = (uint8_t)(v >> 8);
  body[2] = (uint8_t)(v >> 16);
  body[3] = (uint8_t)(v >> 24);
  return 0;
}

static int wnp_bytes(kvspace_t *kv, const char *key, int32_t n, int seed) {
  char lt[32];
  snprintf(lt, sizeof lt, "[%d]uint8", n);
  uint8_t *body;
  if (kvspaceShmWriteNewPlace(kv, key, KVSPACE_REF_INLINE,
                              KVSPACE_STORETYPE_ARRAYND, 0, 0, lt, n,
                              &body) != 0)
    return -1;
  for (int32_t j = 0; j < n; j++)
    body[j] = (uint8_t)(seed + j);
  return 0;
}

static kvspace_t *reopen(const char *db, kvspace_t *kv) {
  kvspaceShmClose(kv);
  return kvspaceShmOpen(db, 8); /* reopen ignores data_size */
}

static void t_set_churn(const char *db) {
  printf("[set] fill, reopen, same-size overwrite, grow/shrink overwrite\n");
  kvspace_t *kv = kvspaceShmOpen(db, DATA_SIZE);
  REQUIRE(kv != NULL, "open failed");
  char key[32];
  int fail = 0;
  for (int i = 0; i < NKEYS; i++) {
    key_of(key, sizeof key, i);
    fail += set_int32(kv, key, i) != 0;
  }
  CHECK(fail == 0, "%d initial sets failed", fail);

  kv = reopen(db, kv);
  REQUIRE(kv != NULL, "reopen after fill failed");
  fail = 0;
  int bad = 0;
  for (int i = 0; i < NKEYS; i++) {
    key_of(key, sizeof key, i);
    fail += set_int32(kv, key, i + 1) != 0;
    bad += get_int32(kv, key) != i + 1;
  }
  CHECK(fail == 0, "%d same-size overwrites failed", fail);
  CHECK(bad == 0, "%d keys wrong after same-size overwrite", bad);

  kv = reopen(db, kv);
  REQUIRE(kv != NULL, "reopen before grow failed");
  for (int r = 0; r < GROW_SIZES; r++) {
    int32_t n = k_grow[r];
    fail = 0;
    bad = 0;
    for (int i = 0; i < NKEYS; i++) {
      key_of(key, sizeof key, i);
      fail += set_bytes(kv, key, n, i + r) != 0;
    }
    CHECK(fail == 0, "round %d: %d grow sets failed (n=%d)", r, fail, n);
    kv = reopen(db, kv);
    REQUIRE(kv != NULL, "reopen in grow round %d failed", r);
    for (int i = 0; i < NKEYS; i += 7) {
      key_of(key, sizeof key, i);
      bad += check_bytes(kv, key, n, i + r) != 0;
    }
    CHECK(bad == 0, "round %d: %d keys wrong after grow reopen", r, bad);
  }
  kvspaceShmClose(kv);
}

/* Fill until sbo_alloc fails, then grow a small key. Free-then-alloc would
 * drop the old box before the failed alloc; the key must still read. */
static void t_preserve(const char *dir) {
  printf("[preserve] failed grow overwrite must keep the old value\n");
  char db[512];
  snprintf(db, sizeof db, "%s/pv", dir);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_SIZE);
  REQUIRE(kv != NULL, "open failed");
  char key[32];
  const int nsmall = 32;
  int fail = 0;
  for (int i = 0; i < nsmall; i++) {
    key_of(key, sizeof key, i);
    fail += set_int32(kv, key, i) != 0;
  }
  CHECK(fail == 0, "%d small sets failed", fail);

  int n = nsmall;
  for (; n < 20000; n++) {
    key_of(key, sizeof key, n);
    if (set_bytes(kv, key, 800, n) != 0)
      break;
  }
  CHECK(n > nsmall + 10, "filled only %d keys", n);
  printf("  filled %d large keys before alloc fail\n", n - nsmall);

  kv = reopen(db, kv);
  REQUIRE(kv != NULL, "reopen before grow-fail failed");
  for (int i = 0; i < nsmall; i++) {
    key_of(key, sizeof key, i);
    CHECK(get_int32(kv, key) == i, "small key %d lost after fill/reopen", i);
  }

  key_of(key, sizeof key, 0);
  int grew = set_bytes(kv, key, 800, 99);
  if (grew != 0)
    CHECK(get_int32(kv, key) == 0, "key 0 lost after failed grow overwrite");
  else
    CHECK(check_bytes(kv, key, 800, 99) == 0, "key 0 grow overwrite wrote junk");

  fail = 0;
  for (int i = 1; i < nsmall; i++) {
    key_of(key, sizeof key, i);
    fail += get_int32(kv, key) != i;
  }
  CHECK(fail == 0, "%d other small keys lost after grow overwrite", fail);
  kvspaceShmClose(kv);
}

static void t_wnp_churn(const char *dir) {
  printf("[wnp] WriteNewPlace reopen overwrite (always-new-box path)\n");
  char db[512];
  snprintf(db, sizeof db, "%s/wnp", dir);
  kvspace_t *kv = kvspaceShmOpen(db, DATA_SIZE);
  REQUIRE(kv != NULL, "open failed");
  char key[32];
  int fail = 0;
  for (int i = 0; i < NKEYS; i++) {
    key_of(key, sizeof key, i);
    fail += wnp_int32(kv, key, i) != 0;
  }
  CHECK(fail == 0, "%d initial WriteNewPlace failed", fail);

  kv = reopen(db, kv);
  REQUIRE(kv != NULL, "reopen failed");
  fail = 0;
  int bad = 0;
  for (int i = 0; i < NKEYS; i++) {
    key_of(key, sizeof key, i);
    fail += wnp_int32(kv, key, i + 3) != 0;
    bad += get_int32(kv, key) != i + 3;
  }
  CHECK(fail == 0, "%d WriteNewPlace same-size overwrites failed", fail);
  CHECK(bad == 0, "%d keys wrong after WriteNewPlace overwrite", bad);

  kv = reopen(db, kv);
  REQUIRE(kv != NULL, "reopen before wnp grow failed");
  for (int r = 0; r < GROW_SIZES; r++) {
    int32_t n = k_grow[r];
    fail = 0;
    bad = 0;
    for (int i = 0; i < NKEYS; i++) {
      key_of(key, sizeof key, i);
      fail += wnp_bytes(kv, key, n, i + 11 + r) != 0;
    }
    CHECK(fail == 0, "wnp round %d: %d failed (n=%d)", r, fail, n);
    kv = reopen(db, kv);
    REQUIRE(kv != NULL, "wnp reopen round %d failed", r);
    for (int i = 0; i < NKEYS; i += 7) {
      key_of(key, sizeof key, i);
      bad += check_bytes(kv, key, n, i + 11 + r) != 0;
    }
    CHECK(bad == 0, "wnp round %d: %d keys wrong", r, bad);
  }
  kvspaceShmClose(kv);
}

int main(int argc, char **argv) {
  signal(SIGALRM, on_alrm);
  alarm(90);

  char tmpl[] = "/tmp/kvspace-reopen-XXXXXX";
  const char *dir = argc > 1 ? argv[1] : mkdtemp(tmpl);
  if (!dir) {
    perror("mkdtemp");
    return 2;
  }
  char db[512];
  snprintf(db, sizeof db, "%s/db", dir);

  t_set_churn(db);
  t_wnp_churn(dir);
  t_preserve(dir);

  if (argc <= 1) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
      fprintf(stderr, "cleanup failed: %s\n", dir);
  }
  printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
