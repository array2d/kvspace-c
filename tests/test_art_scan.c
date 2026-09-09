/* test_art_scan — prefix List/DelTree/CpTree scan the covering subtree (#267) */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SBO_DATA_SIZE (8UL * 64 * 64 * 64 * 64)

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

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
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

static void list_free(char **ns, int32_t n) {
  if (!ns)
    return;
  for (int32_t i = 0; i < n; i++)
    free(ns[i]);
  free(ns);
}

static int list_has(char **ns, int32_t n, const char *s) {
  for (int32_t i = 0; i < n; i++)
    if (strcmp(ns[i], s) == 0)
      return 1;
  return 0;
}

static int do_list(kvspace_t *kv, const char *pfx, char ***on, int32_t *n) {
  *on = NULL;
  *n = 0;
  return kvspaceShmList(kv, pfx, false, 0, on, n);
}

static void t_correct(const char *db) {
  printf("[correct] isolation, nested, compressed prefix, miss, deltree, cptree\n");
  kvspace_t *kv = kvspaceShmOpen(db, SBO_DATA_SIZE);
  REQUIRE(kv != NULL, "open failed");

  CHECK(set_int32(kv, "/lib/a", 1) == 0, "set /lib/a");
  CHECK(set_int32(kv, "/lib/b", 2) == 0, "set /lib/b");
  CHECK(set_int32(kv, "/lib/mod/x", 3) == 0, "set /lib/mod/x");
  CHECK(set_int32(kv, "/d/f/p", 4) == 0, "set /d/f/p");
  CHECK(set_int32(kv, "/d/f/q", 5) == 0, "set /d/f/q");
  CHECK(set_int32(kv, "/d/g", 6) == 0, "set /d/g");
  CHECK(set_int32(kv, "/foo/bar1", 7) == 0, "set /foo/bar1");
  CHECK(set_int32(kv, "/foo/bar2", 8) == 0, "set /foo/bar2");
  CHECK(set_int32(kv, "/foo/zzz", 9) == 0, "set /foo/zzz");

  char **ns;
  int32_t n;

  REQUIRE(do_list(kv, "/d/", &ns, &n) == 0, "list /d/");
  CHECK(n == 2, "list /d/ count %d", n);
  CHECK(list_has(ns, n, "f") && list_has(ns, n, "g"), "list /d/ names");
  list_free(ns, n);

  REQUIRE(do_list(kv, "/d/f/", &ns, &n) == 0, "list /d/f/");
  CHECK(n == 2, "list /d/f/ count %d", n);
  CHECK(list_has(ns, n, "p") && list_has(ns, n, "q"), "list /d/f/ names");
  list_free(ns, n);

  REQUIRE(do_list(kv, "/lib/", &ns, &n) == 0, "list /lib/");
  CHECK(n == 3, "list /lib/ count %d", n);
  CHECK(list_has(ns, n, "a") && list_has(ns, n, "b") && list_has(ns, n, "mod"),
        "list /lib/ names");
  list_free(ns, n);

  REQUIRE(do_list(kv, "/foo/", &ns, &n) == 0, "list /foo/");
  CHECK(n == 3, "list /foo/ count %d", n);
  CHECK(list_has(ns, n, "bar1") && list_has(ns, n, "bar2") &&
            list_has(ns, n, "zzz"),
        "list /foo/ names");
  list_free(ns, n);

  REQUIRE(do_list(kv, "/nope/", &ns, &n) == 0, "list /nope/");
  CHECK(n == 0, "list /nope/ count %d", n);
  list_free(ns, n);

  REQUIRE(do_list(kv, "/food/", &ns, &n) == 0, "list /food/");
  CHECK(n == 0, "list /food/ count %d", n);
  list_free(ns, n);

  REQUIRE(do_list(kv, "/", &ns, &n) == 0, "list /");
  CHECK(list_has(ns, n, "lib") && list_has(ns, n, "d") && list_has(ns, n, "foo"),
        "list / names");
  list_free(ns, n);

  CHECK(kvspaceShmExtindex(kv, "/e/", "/lib/") == 0, "extindex");
  REQUIRE(kvspaceShmList(kv, "/e/", true, 0, &ns, &n) == 0, "list /e/ ex");
  CHECK(list_has(ns, n, "a") && list_has(ns, n, "b") && list_has(ns, n, "mod"),
        "extindex children");
  list_free(ns, n);

  CHECK(kvspaceShmCptree(kv, "/d/f", "/cp") == 0, "cptree");
  REQUIRE(do_list(kv, "/cp/", &ns, &n) == 0, "list /cp/");
  CHECK(n == 2 && list_has(ns, n, "p") && list_has(ns, n, "q"),
        "cptree children");
  list_free(ns, n);
  CHECK(get_int32(kv, "/cp/p") == 4 && get_int32(kv, "/cp/q") == 5,
        "cptree values");

  CHECK(kvspaceShmDeltree(kv, "/d") == 0, "deltree /d");
  REQUIRE(do_list(kv, "/d/", &ns, &n) == 0, "list /d/ after deltree");
  CHECK(n == 0, "deltree left children %d", n);
  list_free(ns, n);
  CHECK(get_int32(kv, "/lib/a") == 1 && get_int32(kv, "/foo/bar1") == 7,
        "deltree escaped sibling trees");
  CHECK(get_int32(kv, "/d/f/p") == -1, "deltree did not remove target");

  kvspaceShmClose(kv);
}

static int fill_noise(kvspace_t *kv, int from, int to) {
  char key[32];
  int fail = 0;
  for (int i = from; i < to; i++) {
    snprintf(key, sizeof key, "/n/%02d/%03d", i / 100, i % 100);
    fail += set_int32(kv, key, i) != 0;
  }
  return fail;
}

static double time_list(kvspace_t *kv, const char *pfx, int reps) {
  for (int i = 0; i < 20; i++) {
    char **ns;
    int32_t n;
    do_list(kv, pfx, &ns, &n);
    list_free(ns, n);
  }
  double t0 = now_s();
  for (int i = 0; i < reps; i++) {
    char **ns;
    int32_t n;
    do_list(kv, pfx, &ns, &n);
    list_free(ns, n);
  }
  return now_s() - t0;
}

static void t_scale(const char *db) {
  const int lo = 1000, hi = 8000, reps = 400;
  printf("[scale] List(/t/) vs sibling tree size %d -> %d, %d reps\n", lo, hi,
         reps);
  kvspace_t *kv = kvspaceShmOpen(db, SBO_DATA_SIZE);
  REQUIRE(kv != NULL, "open failed");

  CHECK(set_int32(kv, "/t/a", 1) == 0, "set /t/a");
  CHECK(set_int32(kv, "/t/b", 2) == 0, "set /t/b");
  CHECK(set_int32(kv, "/t/c/x", 3) == 0, "set /t/c/x");
  CHECK(fill_noise(kv, 0, lo) == 0, "fill lo noise");

  char **ns;
  int32_t n;
  REQUIRE(do_list(kv, "/t/", &ns, &n) == 0, "list /t/");
  CHECK(n == 3 && list_has(ns, n, "a") && list_has(ns, n, "b") &&
            list_has(ns, n, "c"),
        "list /t/ names");
  list_free(ns, n);

  double t_lo = time_list(kv, "/t/", reps);
  CHECK(fill_noise(kv, lo, hi) == 0, "fill hi noise");
  REQUIRE(do_list(kv, "/t/", &ns, &n) == 0, "list /t/ after noise");
  CHECK(n == 3, "list /t/ count after noise %d", n);
  list_free(ns, n);
  double t_hi = time_list(kv, "/t/", reps);
  double ratio = t_lo > 0 ? t_hi / t_lo : 0;

  printf("  list /t/  %d noise: %.4fs\n", lo, t_lo);
  printf("  list /t/  %d noise: %.4fs  ratio=%.2f (must not track noise)\n", hi,
         t_hi, ratio);
  CHECK(ratio < 2.5, "List(/t/) scaled with sibling size: %.2fx", ratio);

  kvspaceShmClose(kv);
}

int main(int argc, char **argv) {
  char tmpl[] = "/tmp/kvspace-artscan-XXXXXX";
  const char *dir = argc > 1 ? argv[1] : mkdtemp(tmpl);
  if (!dir) {
    perror("mkdtemp");
    return 2;
  }
  char db[512], db2[512];
  snprintf(db, sizeof db, "%s/db", dir);
  snprintf(db2, sizeof db2, "%s/db2", dir);

  t_correct(db);
  t_scale(db2);

  if (argc <= 1) {
    char cmd[700];
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", dir);
    if (system(cmd) != 0)
      fprintf(stderr, "cleanup failed: %s\n", dir);
  }
  printf(failures ? "FAILED (%d)\n" : "OK\n", failures);
  return failures ? 1 : 0;
}
