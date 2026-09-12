#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "xvalue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DATA_SIZE (8UL * 64 * 64)
#define CHECK(c)                                                               \
  do {                                                                         \
    if (!(c)) {                                                                \
      failures++;                                                              \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
    }                                                                          \
  } while (0)

static int failures;

static int set_i64(kvspace_t *kv, const char *key, int64_t v) {
  uint8_t *tlv;
  int32_t n = kvspaceXvalueNewInt64(&v, 1, &tlv);
  if (n < 0)
    return -1;
  int rc = kvspaceShmSet(kv, key, tlv, n);
  free(tlv);
  return rc;
}

static int64_t get_i64(const uint8_t *d, int32_t len) {
  if (!d || len <= 0)
    return -999;
  xvalue_head_t h = kvspaceXvalueDecodeHead(d, len);
  return h.raw_len == 8 ? kvspaceXvalueRawInt64(h.raw) : -999;
}

static uint64_t nsec(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

int main(void) {
  char dir[] = "/tmp/kvs-ref-XXXXXX";
  if (!mkdtemp(dir))
    return 1;
  char path[256];
  snprintf(path, sizeof path, "%s/s", dir);
  kvspace_t *kv = kvspaceShmOpen(path, DATA_SIZE);
  if (!kv) {
    fprintf(stderr, "open failed\n");
    return 1;
  }

  CHECK(set_i64(kv, "/a", 1) == 0);
  kvspaceRef_t ref;
  CHECK(kvspaceShmResolveRef(kv, "/a", &ref) == 0);
  int32_t len = 0;
  uint8_t *d = kvspaceShmGetByRef(kv, &ref, "/a", &len);
  CHECK(get_i64(d, len) == 1);
  uint32_t id0 = ref.block_id;

  CHECK(set_i64(kv, "/a", 42) == 0);
  d = kvspaceShmGetByRef(kv, &ref, "/a", &len);
  CHECK(get_i64(d, len) == 42);
  CHECK(ref.block_id == id0);

  CHECK(set_i64(kv, "/n", 7) == 0);
  kvspaceRef_t nr;
  CHECK(kvspaceShmResolveRef(kv, "/n", &nr) == 0);
  for (int i = 0; i < 5; i++) {
    char k[8];
    snprintf(k, sizeof k, "/n%d", i);
    CHECK(set_i64(kv, k, i) == 0);
  }
  d = kvspaceShmGetByRef(kv, &nr, "/n", &len);
  CHECK(get_i64(d, len) == 7);

  kvspaceShmDel(kv, "/a");
  len = 0;
  d = kvspaceShmGetByRef(kv, &ref, NULL, &len);
  CHECK(d == NULL);
  CHECK(set_i64(kv, "/a", 9) == 0);
  d = kvspaceShmGetByRef(kv, &ref, "/a", &len);
  CHECK(get_i64(d, len) == 9);

  CHECK(set_i64(kv, "/p", 100) == 0);
  kvspaceRef_t pr;
  CHECK(kvspaceShmResolveRef(kv, "/p", &pr) == 0);
  d = kvspaceShmGet(kv, "/p", 0, &len);
  CHECK(d && len > 8);
  xvalue_head_t hh = kvspaceXvalueDecodeHead(d, len);
  int64_t nv = 200;
  uint8_t raw[8];
  for (int i = 0; i < 8; i++)
    raw[i] = (uint8_t)((uint64_t)nv >> (8 * i));
  CHECK(kvspaceShmSetPartByRef(kv, &pr, "/p",
                               (uint32_t)kvspaceXvalueHeadLen(&hh), raw,
                               8) == 0);
  d = kvspaceShmGetByRef(kv, &pr, "/p", &len);
  CHECK(get_i64(d, len) == 200);

  for (int i = 0; i < 64; i++) {
    char k[16];
    snprintf(k, sizeof k, "/sib/%02d", i);
    CHECK(set_i64(kv, k, i + 100) == 0);
  }
  for (int i = 0; i < 64; i++) {
    char k[16];
    snprintf(k, sizeof k, "/sib/%02d", i);
    int32_t l = 0;
    d = kvspaceShmGet(kv, k, 0, &l);
    CHECK(get_i64(d, l) == i + 100);
  }
  CHECK(set_i64(kv, "/other", 1) == 0);
  {
    int32_t l = 0;
    d = kvspaceShmGet(kv, "/sib/00", 0, &l);
    CHECK(get_i64(d, l) == 100);
    d = kvspaceShmGet(kv, "/other", 0, &l);
    CHECK(get_i64(d, l) == 1);
    kvspaceRef_t sr;
    CHECK(set_i64(kv, "/frm/a", 1) == 0);
    CHECK(set_i64(kv, "/frm/i", 2) == 0);
    CHECK(set_i64(kv, "/frm/n", 3) == 0);
    CHECK(kvspaceShmResolveRef(kv, "/frm/a", &sr) == 0);
    CHECK(sr.depth > 0 && sr.parent_id != 0);
    d = kvspaceShmGetByRef(kv, &sr, "/frm/a", &l);
    CHECK(get_i64(d, l) == 1);
    {
      kvspaceRef_t pr = { sr.parent_id, sr.depth, 0, 0 };
      d = kvspaceShmGetByRef(kv, &pr, "/frm/i", &l);
      CHECK(get_i64(d, l) == 2);
      CHECK(get_i64(d, l) != 1);
      d = kvspaceShmGetByRef(kv, &pr, "/frm/n", &l);
      CHECK(get_i64(d, l) == 3);
      CHECK(get_i64(d, l) != 1);
    }
  }

  const int N = 200000;
  CHECK(set_i64(kv, "/hot", 1) == 0);
  kvspaceRef_t hr;
  CHECK(kvspaceShmResolveRef(kv, "/hot", &hr) == 0);
  uint64_t t0 = nsec();
  for (int i = 0; i < N; i++) {
    int32_t l = 0;
    kvspaceShmGet(kv, "/hot", 0, &l);
  }
  uint64_t t1 = nsec();
  for (int i = 0; i < N; i++) {
    int32_t l = 0;
    kvspaceShmGetByRef(kv, &hr, "/hot", &l);
  }
  uint64_t t2 = nsec();
  double ns_get = (double)(t1 - t0) / N;
  double ns_ref = (double)(t2 - t1) / N;
  printf("Get %.1f ns/op  GetByRef %.1f ns/op  ratio %.2f\n", ns_get, ns_ref,
         ns_ref > 0 ? ns_get / ns_ref : 0);

  const char *fk[] = {"/frm/a", "/frm/i", "/frm/n"};
  const int NF = 3;
  const int NR = 50000;
  uint64_t t3 = nsec();
  for (int r = 0; r < NR; r++) {
    int32_t l = 0;
    kvspaceShmGet(kv, fk[r % NF], 0, &l);
  }
  uint64_t t4 = nsec();
  kvspaceRef_t sr, pref;
  CHECK(kvspaceShmResolveRef(kv, fk[0], &sr) == 0);
  CHECK(sr.depth > 0);
  pref.block_id = sr.parent_id;
  pref.gen = sr.depth;
  pref.parent_id = 0;
  pref.depth = 0;
  uint64_t t5 = nsec();
  for (int r = 0; r < NR; r++) {
    int32_t l = 0;
    kvspaceShmGetByRef(kv, &pref, fk[r % NF], &l);
  }
  uint64_t t6 = nsec();
  printf("Get sibling %.1f ns/op  GetByRef sibling %.1f ns/op  ratio %.2f\n",
         (double)(t4 - t3) / NR, (double)(t6 - t5) / NR,
         (t6 - t5) > 0 ? (double)(t4 - t3) / (double)(t6 - t5) : 0);

  kvspaceShmClose(kv);
  return failures ? 1 : 0;
}
