/*
 * kvspace.c — KVSpace SHM 存储引擎: ART 树 + slotsboxmalloc
 */

#define _GNU_SOURCE
#include "kvspace_shm.h"
#include "slotsboxmalloc/slotsboxobj.h"
#include <blockmalloc/blockmalloc.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define KVS_MAGIC "kvspace-c.v1"
#define ART_PREFIX_MAX 10
#define ART_NODE_MAX_SZ 2112
#define ART_SLAB_SZ (256UL * 1024 * 1024)
#define WATCH_TABLE_SZ 256

enum { ART_N4 = 0, ART_N16 = 1, ART_N48 = 2, ART_N256 = 3 };

typedef struct {
  uint8_t type, prefix[ART_PREFIX_MAX], prefix_len;
  uint8_t has_value : 1;
  uint64_t box_offset : 63;
  uint16_t count;
} art_hdr_t;

typedef struct {
  art_hdr_t h;
  uint8_t keys[4];
  int32_t children[4];
} art_n4_t;
typedef struct {
  art_hdr_t h;
  uint8_t keys[16];
  int32_t children[16];
} art_n16_t;
typedef struct {
  art_hdr_t h;
  uint8_t index[256];
  int32_t children[48];
} art_n48_t;
typedef struct {
  art_hdr_t h;
  int32_t children[256];
} art_n256_t;

static int art_node_sz(int t) {
  return t == ART_N4    ? sizeof(art_n4_t)
         : t == ART_N16 ? sizeof(art_n16_t)
         : t == ART_N48 ? sizeof(art_n48_t)
                        : sizeof(art_n256_t);
}

typedef struct {
  char magic[12];
  uint64_t shm_size, sbo_head_size, sbo_data_offset, sbo_data_size,
      art_slab_size;
  int32_t art_root;
} kvspace_hdr_t;

typedef struct {
  char key[256];
  pthread_cond_t cond;
  pthread_mutex_t mtx;
  uint8_t *val;
  int32_t val_len;
  bool ready;
} watch_t;

struct kvspace {
  int fd;
  size_t shm_sz;
  uint8_t *shm;
  kvspace_hdr_t *hdr;
  blocks_meta_t *art_meta;
  uint8_t *art_data;
  uint8_t *sbo_meta, *sbo_data;
  watch_t watches[WATCH_TABLE_SZ];
  pthread_mutex_t wlock;
};

/* ---- helpers ---- */
static void *art_blk(kvspace_t *kv, int32_t id) {
  if (id < 0)
    return NULL;
  return kv->art_data + blockdata_offset(kv->art_meta, (uint64_t)id);
}
static int32_t art_balloc(kvspace_t *kv) {
  return (int32_t)blocks_alloc(kv->art_meta, kv->art_data);
}
static art_hdr_t *art_hdr(kvspace_t *kv, int32_t id) {
  return (art_hdr_t *)art_blk(kv, id);
}

/* ---- prefix ---- */
static int pfx_shared(const uint8_t *a, int al, const uint8_t *b, int bl) {
  int n = al < bl ? al : bl;
  for (int i = 0; i < n; i++)
    if (a[i] != b[i])
      return i;
  return n;
}

/* ---- child lookup ---- */
static int32_t art_child(kvspace_t *kv, void *n, uint8_t b) {
  art_hdr_t *h = (art_hdr_t *)n;
  switch (h->type) {
  case ART_N4: {
    art_n4_t *x = n;
    for (int i = 0; i < (int)h->count; i++)
      if (x->keys[i] == b)
        return x->children[i];
    return -1;
  }
  case ART_N16: {
    art_n16_t *x = n;
    int lo = 0, hi = (int)h->count - 1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (x->keys[m] == b)
        return x->children[m];
      if (x->keys[m] < b)
        lo = m + 1;
      else
        hi = m - 1;
    }
    return -1;
  }
  case ART_N48: {
    art_n48_t *x = n;
    uint8_t idx = x->index[b];
    return idx == 255 ? -1 : x->children[idx];
  }
  case ART_N256: {
    return ((art_n256_t *)n)->children[b];
  }
  }
  return -1;
}

/* ---- art_search ---- */
static art_hdr_t *art_search(kvspace_t *kv, int32_t nid, const uint8_t *key,
                             int klen) {
  if (nid < 0 || !key)
    return NULL;
  int d = 0;
  while (nid >= 0) {
    art_hdr_t *h = art_hdr(kv, nid);
    if (!h)
      return NULL;
    if (h->prefix_len) {
      int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
      if (s != h->prefix_len) {
        if (d + s < klen)
          return NULL;
        if (s < h->prefix_len)
          return NULL;
      }
      d += h->prefix_len;
      if (d > klen)
        return NULL;
    }
    if (d == klen)
      return h->has_value ? h : NULL;
    nid = art_child(kv, h, key[d]);
    d++;
  }
  return NULL;
}

/* ---- node create ---- */
static int32_t art_new_leaf(kvspace_t *kv, uint64_t off) {
  int32_t id = art_balloc(kv);
  if (id < 0)
    return -1;
  art_n4_t *x = art_blk(kv, id);
  memset(x, 0, sizeof(*x));
  x->h.type = ART_N4;
  x->h.has_value = 1;
  x->h.box_offset = off;
  return id;
}
static int32_t art_new_node(kvspace_t *kv, int t) {
  int32_t id = art_balloc(kv);
  if (id < 0)
    return -1;
  void *x = art_blk(kv, id);
  int sz = art_node_sz(t);
  memset(x, 0, sz);
  ((art_hdr_t *)x)->type = (uint8_t)t;
  if (t == ART_N48)
    memset(((art_n48_t *)x)->index, 255, 256);
  if (t == ART_N256) {
    art_n256_t *n = x;
    for (int i = 0; i < 256; i++)
      n->children[i] = -1;
  }
  return id;
}

/* ---- grow ---- */
static int32_t art_grow(kvspace_t *kv, void *on) {
  art_hdr_t *oh = on;
  int nt;
  if (oh->type == ART_N4)
    nt = ART_N16;
  else if (oh->type == ART_N16)
    nt = ART_N48;
  else if (oh->type == ART_N48)
    nt = ART_N256;
  else
    return -1;
  int32_t nid = art_new_node(kv, nt);
  if (nid < 0)
    return -1;
  art_hdr_t *nh = art_hdr(kv, nid);
  nh->prefix_len = oh->prefix_len;
  memcpy(nh->prefix, oh->prefix, oh->prefix_len);
  nh->has_value = oh->has_value;
  nh->box_offset = oh->box_offset;
  // copy children
  for (int i = 0; i < (int)oh->count; i++) {
    uint8_t b = 0;
    int32_t c = -1;
    switch (oh->type) {
    case ART_N4: {
      art_n4_t *x = on;
      b = x->keys[i];
      c = x->children[i];
      break;
    }
    case ART_N16: {
      art_n16_t *x = on;
      b = x->keys[i];
      c = x->children[i];
      break;
    }
    case ART_N48: {
      art_n48_t *x = on;
      c = x->children[i];
      for (int j = 0; j < 256; j++)
        if (x->index[j] == i) {
          b = (uint8_t)j;
          break;
        }
      break;
    }
    }
    switch (nt) {
    case ART_N16: {
      art_n16_t *x = (art_n16_t *)nh;
      x->keys[i] = b;
      x->children[i] = c;
      break;
    }
    case ART_N48: {
      art_n48_t *x = (art_n48_t *)nh;
      x->index[b] = (uint8_t)i;
      x->children[i] = c;
      break;
    }
    case ART_N256: {
      ((art_n256_t *)nh)->children[b] = c;
      break;
    }
    }
  }
  nh->count = oh->count;
  if (nt == ART_N16) {
    art_n16_t *x = (art_n16_t *)nh;
    for (int i = 0; i < (int)nh->count - 1; i++)
      for (int j = i + 1; j < (int)nh->count; j++)
        if (x->keys[i] > x->keys[j]) {
          uint8_t tk = x->keys[i];
          x->keys[i] = x->keys[j];
          x->keys[j] = tk;
          int32_t tc = x->children[i];
          x->children[i] = x->children[j];
          x->children[j] = tc;
        }
  }
  return nid;
}

/* ---- add child ---- */
static int art_add(kvspace_t *kv, void *n, uint8_t b, int32_t cid) {
  art_hdr_t *h = n;
  switch (h->type) {
  case ART_N4: {
    if (h->count >= 4)
      return -1;
    art_n4_t *x = n;
    x->keys[h->count] = b;
    x->children[h->count] = cid;
    h->count++;
    return 0;
  }
  case ART_N16: {
    if (h->count >= 16)
      return -1;
    art_n16_t *x = n;
    int p = (int)h->count;
    while (p > 0 && x->keys[p - 1] > b) {
      x->keys[p] = x->keys[p - 1];
      x->children[p] = x->children[p - 1];
      p--;
    }
    x->keys[p] = b;
    x->children[p] = cid;
    h->count++;
    return 0;
  }
  case ART_N48: {
    if (h->count >= 48)
      return -1;
    art_n48_t *x = n;
    int s = (int)h->count;
    x->index[b] = (uint8_t)s;
    x->children[s] = cid;
    h->count++;
    return 0;
  }
  case ART_N256: {
    art_n256_t *x = n;
    if (x->children[b] >= 0)
      return -1;
    x->children[b] = cid;
    h->count++;
    return 0;
  }
  }
  return -1;
}

/* ---- remove child ---- */
static void art_rm(kvspace_t *kv, void *n, uint8_t b) {
  art_hdr_t *h = n;
  switch (h->type) {
  case ART_N4: {
    art_n4_t *x = n;
    for (int i = 0; i < (int)h->count; i++)
      if (x->keys[i] == b) {
        for (int j = i; j < (int)h->count - 1; j++) {
          x->keys[j] = x->keys[j + 1];
          x->children[j] = x->children[j + 1];
        }
        h->count--;
        return;
      }
    break;
  }
  case ART_N16: {
    art_n16_t *x = n;
    int lo = 0, hi = (int)h->count - 1, pos = -1;
    while (lo <= hi) {
      int m = (lo + hi) / 2;
      if (x->keys[m] == b) {
        pos = m;
        break;
      }
      if (x->keys[m] < b)
        lo = m + 1;
      else
        hi = m - 1;
    }
    if (pos < 0)
      return;
    for (int j = pos; j < (int)h->count - 1; j++) {
      x->keys[j] = x->keys[j + 1];
      x->children[j] = x->children[j + 1];
    }
    h->count--;
    break;
  }
  case ART_N48: {
    art_n48_t *x = n;
    uint8_t idx = x->index[b];
    if (idx == 255)
      return;
    x->index[b] = 255;
    int last = (int)h->count - 1;
    if (idx != last) {
      x->children[idx] = x->children[last];
      for (int j = 0; j < 256; j++)
        if (x->index[j] == last) {
          x->index[j] = idx;
          break;
        }
    }
    h->count--;
    break;
  }
  case ART_N256: {
    art_n256_t *x = n;
    x->children[b] = -1;
    h->count--;
    break;
  }
  }
}

/* ---- 叶子链：key[d..klen) 建链，末端叶子存 off（尾段超 ART_PREFIX_MAX 分级） ---- */
static int32_t art_leaf_chain(kvspace_t *kv, const uint8_t *key, int klen,
                              int d, uint64_t off) {
  int rem = klen - d;
  if (rem <= ART_PREFIX_MAX) {
    int32_t id = art_new_leaf(kv, off);
    if (id < 0)
      return -1;
    art_n4_t *x = art_blk(kv, id);
    if (rem > 0)
      memcpy(x->h.prefix, key + d, (size_t)rem);
    x->h.prefix_len = (uint8_t)rem;
    return id;
  }
  int32_t id = art_new_node(kv, ART_N4);
  if (id < 0)
    return -1;
  art_hdr_t *x = art_hdr(kv, id);
  memcpy(x->prefix, key + d, ART_PREFIX_MAX);
  x->prefix_len = ART_PREFIX_MAX;
  int32_t sub = art_leaf_chain(kv, key, klen, d + ART_PREFIX_MAX + 1, off);
  if (sub < 0)
    return -1;
  art_add(kv, x, key[d + ART_PREFIX_MAX], sub);
  return id;
}

/* ---- insert ---- */
static int32_t art_ins(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                       int d, uint64_t off) {
  if (nid < 0)
    return art_leaf_chain(kv, key, klen, d, off);

  art_hdr_t *h = art_hdr(kv, nid);
  if (!h)
    return -1;
  int shared = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
  int mpl = h->prefix_len < (klen - d) ? h->prefix_len : (klen - d);

  if (shared == mpl && (klen - d) < h->prefix_len) {
    /* key 在节点 prefix 内部耗尽（含 d==klen）：拆出 shared 字节作新值，旧节点作 child */
    int32_t nn = art_new_node(kv, ART_N4);
    if (nn < 0)
      return -1;
    art_hdr_t *nh = art_hdr(kv, nn);
    nh->prefix_len = (uint8_t)shared;
    if (shared > 0)
      memcpy(nh->prefix, key + d, (size_t)shared);
    uint8_t ob = h->prefix[shared];
    h->prefix_len -= (uint8_t)(shared + 1);
    if (h->prefix_len > 0)
      memmove(h->prefix, h->prefix + shared + 1, h->prefix_len);
    art_add(kv, nh, ob, nid);
    nh->has_value = 1;
    nh->box_offset = off;
    return nn;
  }
  if (shared < mpl) { // prefix split
    int32_t nn = art_new_node(kv, ART_N4);
    if (nn < 0)
      return -1;
    art_hdr_t *nh = art_hdr(kv, nn);
    nh->prefix_len = (uint8_t)shared;
    memcpy(nh->prefix, key + d, shared);
    uint8_t ob = h->prefix[shared];
    h->prefix_len -= (uint8_t)(shared + 1);
    if (h->prefix_len > 0)
      memmove(h->prefix, h->prefix + shared + 1, h->prefix_len);
    art_add(kv, nh, ob, nid);
    int32_t leaf = art_leaf_chain(kv, key, klen, d + shared + 1, off);
    if (leaf < 0)
      return -1;
    art_add(kv, nh, key[d + shared], leaf);
    return nn;
  }
  d += h->prefix_len;
  if (d == klen) {
    if (h->has_value)
      h->box_offset = off;
    else {
      h->has_value = 1;
      h->box_offset = off;
    }
    return nid;
  }
  int32_t cid = art_child(kv, h, key[d]);
  if (cid >= 0) {
    int32_t nc = art_ins(kv, cid, key, klen, d + 1, off);
    if (nc < 0)
      return -1;
    if (nc != cid) {
      art_rm(kv, h, key[d]);
      art_add(kv, h, key[d], nc);
    }
    return nid;
  }
  int32_t leaf = art_leaf_chain(kv, key, klen, d + 1, off);
  if (leaf < 0)
    return -1;
  if ((h->type == ART_N4 && h->count >= 4) ||
      (h->type == ART_N16 && h->count >= 16) ||
      (h->type == ART_N48 && h->count >= 48)) {
    int32_t g = art_grow(kv, h);
    if (g < 0)
      return -1;
    art_add(kv, art_hdr(kv, g), key[d], leaf);
    return g;
  }
  art_add(kv, h, key[d], leaf);
  return nid;
}

/* ---- delete ---- */
static int32_t art_del(kvspace_t *kv, int32_t nid, const uint8_t *key, int klen,
                       int d, bool *del) {
  if (nid < 0)
    return -1;
  art_hdr_t *h = art_hdr(kv, nid);
  if (!h)
    return -1;
  if (h->prefix_len) {
    int s = pfx_shared(h->prefix, h->prefix_len, key + d, klen - d);
    if (s != h->prefix_len || d + h->prefix_len > klen)
      return nid;
    d += h->prefix_len;
  }
  if (d == klen) {
    if (!h->has_value)
      return nid;
    h->has_value = 0;
    sbo_free(kv->sbo_meta, h->box_offset);
    h->box_offset = 0;
    *del = true;
  } else {
    int32_t cid = art_child(kv, h, key[d]);
    if (cid >= 0)
      art_del(kv, cid, key, klen, d + 1, del);
  }
  return nid;
}

/* ---- path ---- */
static char *pjoin(const char *a, const char *b) {
  size_t al = strlen(a), bl = strlen(b);
  int sep = (al > 0 && a[al - 1] != '/') ? 1 : 0;
  char *r = malloc(al + sep + bl + 1);
  memcpy(r, a, al);
  if (sep)
    r[al] = '/';
  memcpy(r + al + sep, b, bl + 1);
  return r;
}
static void psplit(const char *k, char **p, char **n) {
  const char *s = strrchr(k, '/');
  if (!s || s == k) {
    *p = strdup("/");
    *n = strdup(s ? s + 1 : k);
    return;
  }
  size_t pl = (size_t)(s - k) + 1; /* 含尾 '/' */
  *p = malloc(pl + 1);
  memcpy(*p, k, pl);
  (*p)[pl] = '\0';
  *n = strdup(s + 1);
}
/* 非法目录前缀（不是 / 且不以 / 或 · 结尾）→ 返回非 0，对齐 durable validate_dir。 */
static int bad_dir_prefix(const char *p) {
  if (!p || !p[0])
    return 1;
  if (strcmp(p, "/") == 0)
    return 0;
  size_t l = strlen(p);
  if (p[l - 1] == '/')
    return 0;
  if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 && (unsigned char)p[l - 1] == 0xB7)
    return 0;
  return 1;
}

static char *edir(const char *p) {
  size_t l = strlen(p);
  if (l > 0 && p[l - 1] == '/')
    return strdup(p);
  char *r = malloc(l + 2);
  memcpy(r, p, l);
  r[l] = '/';
  r[l + 1] = '\0';
  return r;
}
/* ---- prefix scan: collect all keys under prefix into out[0..*n-1] ---- */
static void art_scan(kvspace_t *kv, int32_t nid, char *buf, int bpos, int bcap,
                     const char *pfx, int plen, char ***out, int32_t *n) {
  if (nid < 0 || *n >= 4096)
    return;
  art_hdr_t *h = art_hdr(kv, nid);
  if (!h)
    return;
  // write node's prefix into buf
  for (int i = 0; i < h->prefix_len && bpos < bcap; i++)
    buf[bpos++] = h->prefix[i];
  if (bpos >= bcap)
    return;
  // if this node has value, emit key
  if (h->has_value) {
    buf[bpos] = '\0';
    if (bpos >= plen && memcmp(buf, pfx, plen) == 0) {
      (*out)[*n] = strdup(buf);
      (*n)++;
    }
  }
  // recurse into children
  switch (h->type) {
  case ART_N4: {
    art_n4_t *x = (art_n4_t *)h;
    for (int i = 0; i < (int)h->count; i++) {
      if (bpos < bcap)
        buf[bpos] = x->keys[i];
      art_scan(kv, x->children[i], buf, bpos + (bpos < bcap ? 1 : 0), bcap, pfx,
               plen, out, n);
    }
    break;
  }
  case ART_N16: {
    art_n16_t *x = (art_n16_t *)h;
    for (int i = 0; i < (int)h->count; i++) {
      if (bpos < bcap)
        buf[bpos] = x->keys[i];
      art_scan(kv, x->children[i], buf, bpos + (bpos < bcap ? 1 : 0), bcap, pfx,
               plen, out, n);
    }
    break;
  }
  case ART_N48: {
    art_n48_t *x = (art_n48_t *)h;
    for (int i = 0; i < 256; i++)
      if (x->index[i] != 255) {
        if (bpos < bcap)
          buf[bpos] = (uint8_t)i;
        art_scan(kv, x->children[x->index[i]], buf,
                 bpos + (bpos < bcap ? 1 : 0), bcap, pfx, plen, out, n);
      }
    break;
  }
  case ART_N256: {
    art_n256_t *x = (art_n256_t *)h;
    for (int i = 0; i < 256; i++)
      if (x->children[i] >= 0) {
        if (bpos < bcap)
          buf[bpos] = (uint8_t)i;
        art_scan(kv, x->children[i], buf, bpos + (bpos < bcap ? 1 : 0), bcap,
                 pfx, plen, out, n);
      }
    break;
  }
  }
}

/* ============ lifecycle ============ */
kvspace_t *kvspaceShmOpen(const char *path, size_t data_size) {
  if (!path || data_size == 0)
    return NULL;

  bool created = false;
  int fd = open(path, O_RDWR);
  if (fd < 0) {
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (fd < 0)
      return NULL;
    created = true;
  }

  if (!created) {
    struct stat st;
    fstat(fd, &st);
    if (st.st_size < (off_t)sizeof(kvspace_hdr_t)) {
      close(fd);
      return NULL;
    }
    kvspace_hdr_t tmp;
    pread(fd, &tmp, sizeof(tmp), 0);
    if (memcmp(tmp.magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1) != 0) {
      close(fd);
      return NULL;
    }
    data_size = (size_t)tmp.sbo_data_size;
  } else {
    uint64_t slots = data_size / 8;
    if (slots == 0 || (slots & (slots - 1)) != 0) {
      close(fd);
      return NULL;
    }
  }

  size_t sbo_head = sbo_meta_size(data_size, 256 * 1024);
  size_t shm_total = sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t) +
                     ART_SLAB_SZ + sbo_head + data_size;
  if (created && ftruncate(fd, (off_t)shm_total) != 0) {
    close(fd);
    return NULL;
  }
  uint8_t *shm =
      mmap(NULL, shm_total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (shm == MAP_FAILED) {
    close(fd);
    return NULL;
  }

  kvspace_t *kv = calloc(1, sizeof(*kv));
  if (!kv) {
    munmap(shm, shm_total);
    close(fd);
    return NULL;
  }
  kv->fd = fd;
  kv->shm_sz = shm_total;
  kv->shm = shm;
  kv->hdr = (kvspace_hdr_t *)shm;
  kv->art_meta = (blocks_meta_t *)(shm + sizeof(kvspace_hdr_t));
  kv->art_data = shm + sizeof(kvspace_hdr_t) + sizeof(blocks_meta_t);
  kv->sbo_meta = kv->art_data + ART_SLAB_SZ;
  kv->sbo_data = kv->sbo_meta + sbo_head;

  if (created) {
    memset(kv->hdr, 0, sizeof(*kv->hdr));
    memcpy(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1);
    kv->hdr->shm_size = shm_total;
    kv->hdr->sbo_head_size = sbo_head;
    kv->hdr->sbo_data_offset = (uint64_t)(kv->sbo_data - shm);
    kv->hdr->sbo_data_size = data_size;
    kv->hdr->art_slab_size = ART_SLAB_SZ;
    kv->hdr->art_root = -1;
    blocks_init(kv->art_meta, ART_SLAB_SZ, ART_NODE_MAX_SZ);
    sbo_init(kv->sbo_meta, sbo_head, (size_t)kv->hdr->sbo_data_size);
  } else {
    if (memcmp(kv->hdr->magic, KVS_MAGIC, sizeof(KVS_MAGIC) - 1) != 0) {
      kvspaceShmClose(kv);
      return NULL;
    }
  }

  pthread_mutex_init(&kv->wlock, NULL);
  for (int i = 0; i < WATCH_TABLE_SZ; i++) {
    pthread_cond_init(&kv->watches[i].cond, NULL);
    pthread_mutex_init(&kv->watches[i].mtx, NULL);
  }
  return kv;
}
void kvspaceShmClose(kvspace_t *kv) {
  if (!kv)
    return;
  for (int i = 0; i < WATCH_TABLE_SZ; i++) {
    pthread_cond_destroy(&kv->watches[i].cond);
    pthread_mutex_destroy(&kv->watches[i].mtx);
    free(kv->watches[i].val);
  }
  pthread_mutex_destroy(&kv->wlock);
  if (kv->shm)
    munmap(kv->shm, kv->shm_sz);
  if (kv->fd >= 0)
    close(kv->fd);
  free(kv);
}

/* ---- link resolve helpers ---- */
static int read_tlv(kvspace_t *kv, uint64_t off, uint8_t **out, int32_t *ol) {
  uint8_t *s = kv->sbo_data + off;   /* box 内必含完整 TLV，用 xvalue 解码器算长度 */
  size_t sz = sbo_allocated_size(kv->sbo_meta, off);
  xvalue_head_t h = kvspaceXvalueDecodeHead(s, sz > INT32_MAX ? INT32_MAX : (int32_t)sz);
  *out = s; /* SHM pointer */
  if (h.kindexpr_len == 0) { *ol = 0; return 0; } /* None（空 kind）→ len 0 */
  *ol = kvspaceXvalueHeadLen(&h) + h.raw_len;
  return 0;
}
static void resolve_path(kvspace_t *kv, const char *path, char *out, int osz) {
  strncpy(out, path, osz - 1);
  out[osz - 1] = '\0';
  for (int depth = 0; depth < 16; depth++) {
    char cur[1024];
    strncpy(cur, out, sizeof(cur) - 1);
    cur[sizeof(cur) - 1] = '\0';
    bool changed = false;
    // scan / /a /a/b ... for link at each prefix
    char *s = cur;
    while (s && *s) {
      s = strchr(s + 1, '/');
      int pl = s ? (int)(s - cur) : (int)strlen(cur);
      if (pl == 0)
        continue;
      char pre[1024];
      memcpy(pre, cur, pl);
      pre[pl] = '\0';
      art_hdr_t *h =
          art_search(kv, kv->hdr->art_root, (const uint8_t *)pre, pl);
      if (!h || !h->has_value) {
        pre[pl] = '/';
        pre[pl + 1] = '\0';
        h = art_search(kv, kv->hdr->art_root, (const uint8_t *)pre, pl + 1);
      }
      if (!h || !h->has_value) {
        if (s)
          continue; // full key: also try dir form
        char d[1028];
        snprintf(d, sizeof(d), "%s/", cur);
        h = art_search(kv, kv->hdr->art_root, (const uint8_t *)d,
                       (int)strlen(d));
      }
      if (!h || !h->has_value)
        continue;
      uint8_t *raw;
      int32_t rl;
      if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
        continue;
      xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
      if (hh.ref != 1) {
        continue;
      }
      int tl = hh.raw_len;
      if (tl >= osz) {
        break;
      }
      memcpy(out, hh.raw, tl);
      out[tl] = '\0';
      const char *rest = path + pl;
      if (*rest == '/')
        rest++;
      if (*rest) {
        size_t ol = strlen(out);
        if (ol > 0 && out[ol - 1] != '/') {
          out[ol] = '/';
          out[ol + 1] = '\0';
        }
        strncat(out, rest, osz - (int)strlen(out) - 1);
      }
      changed = true;
      break;
    }
    if (!changed)
      break;
  }
}

/* 解析 extindex body 的 extpath（body = [4B count LE]"…extpath\nchild..."）。 */
static void decode_ext_path(const uint8_t *raw, int32_t rl, char *out, int osz) {
  out[0] = 0;
  if (!raw || rl < 4)
    return;
  const char *s = (const char *)raw + 4; /* 跳过 [4B count LE] */
  rl -= 4;
  int start = 0;
  if (rl >= 3 && (uint8_t)s[0] == 0xE2 && (uint8_t)s[1] == 0x80 && (uint8_t)s[2] == 0xA6)
    start = 3; /* 跳过 EXT_PREFIX "…" */
  int i;
  for (i = start; i < rl && s[i] != '\n'; i++)
    ;
  int el = i - start;
  if (el >= osz)
    el = osz - 1;
  memcpy(out, s + start, (size_t)el);
  out[el] = 0;
}

/* 读 dir（尾斜杠目录键）的 extindex，返回 extpath；非 extindex 返回 0。 */
static int dir_ext_path(kvspace_t *kv, const char *dir, char *out, int osz) {
  out[0] = 0;
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)strlen(dir));
  if (!h || !h->has_value)
    return 0;
  uint8_t *raw;
  int32_t rl;
  if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
    return 0;
  xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
  if (hh.kind_len != (int32_t)strlen(KVSPACE_KIND_EXT_INDEX) || memcmp(hh.kind, KVSPACE_KIND_EXT_INDEX, hh.kind_len) != 0)
    return 0;
  decode_ext_path(hh.raw, hh.raw_len, out, osz);
  return out[0] ? 1 : 0;
}

/* ============ CRUD ============ */
uint8_t *kvspaceShmGet(kvspace_t *kv, const char *key, int resolve, int32_t *ol) {
  if (!kv || !key || !ol)
    return NULL;
  *ol = 0;
  char kbuf[1024];
  if (resolve)
    resolve_path(kv, key, kbuf, sizeof(kbuf));
  else {
    strncpy(kbuf, key, sizeof(kbuf) - 1);
    kbuf[sizeof(kbuf) - 1] = '\0';
  }
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                            (int)strlen(kbuf));
  if (!h || !h->has_value) {
    /* extindex fallback：父目录是 extindex → 读 extpath + name */
    char *parent = NULL, *name = NULL;
    psplit(kbuf, &parent, &name);
    char extpath[1024];
    if (dir_ext_path(kv, parent, extpath, sizeof extpath)) {
      char *target = pjoin(extpath, name);
      art_hdr_t *eh = art_search(kv, kv->hdr->art_root, (const uint8_t *)target,
                                 (int)strlen(target));
      free(target);
      if (eh && eh->has_value) {
        uint8_t *raw;
        int32_t rl;
        if (read_tlv(kv, eh->box_offset, &raw, &rl) == 0) {
          *ol = rl;
          free(parent); free(name);
          return raw;
        }
      }
    }
    free(parent); free(name);
    return NULL;
  }
  uint8_t *raw;
  int32_t rl;
  if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
    return NULL;
  *ol = rl;
  return raw;
}

/* ── 值/索引分离（方案2，对齐 kvspace-durable backend.rs） ────────── */
/* 坐标段工具（定义在下方，先声明）。 */
static int coord_is_coord(const char *name);
static int parse_coord(const char *name, int64_t *coords, int maxn);

/* kind 判定（kindexpr 非 NUL 终止）。 */
static int is_kind(const xvalue_head_t *h, const char *k) {
  int32_t kl = (int32_t)strlen(k);
  return h->kind_len == kl && memcmp(h->kind, k, (size_t)kl) == 0;
}

/* 原始落盘（不处理容器/member 语义，供内部调用，避免递归）。 */
static int shm_set_raw(kvspace_t *kv, const char *key, const uint8_t *val,
                       int32_t val_len) {
  art_hdr_t *old = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key));
  if (old && old->has_value) {
    /* 同尺寸原地覆写：新值 ≤ 旧 box 容量时直接 memcpy，跳过 free+alloc，
       不改共享 buddy 树。容量读自共享 mmap，零进程内状态。 */
    uint64_t cap = sbo_allocated_size(kv->sbo_meta, old->box_offset);
    if ((uint64_t)val_len <= cap) {
      memcpy(kv->sbo_data + old->box_offset, val, (size_t)val_len);
      return 0;
    }
    sbo_free(kv->sbo_meta, old->box_offset);
  }
  uint64_t off = sbo_alloc(kv->sbo_meta, (size_t)val_len);
  if (off == (uint64_t)-1)
    return -1;
  memcpy(kv->sbo_data + off, val, val_len);
  kv->hdr->art_root = art_ins(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key), 0, off);
  return 0;
}

/* 去掉尾部分隔符（/ 或 ·），返回 malloc；根 "/" 保持 "/"。 */
static char *strip_dir_suf_alloc(const char *p) {
  size_t l = strlen(p);
  if (l == 0)
    return strdup(p);
  if (l >= 2 && (unsigned char)p[l - 2] == 0xC2 && (unsigned char)p[l - 1] == 0xB7)
    return strndup(p, l - 2);
  if (p[l - 1] == '/') {
    if (l == 1)
      return strdup("/");
    return strndup(p, l - 1);
  }
  return strdup(p);
}

/* base + "·"（memindex 键）。 */
static char *memjoin(const char *base) {
  size_t l = strlen(base);
  char *r = malloc(l + 3);
  memcpy(r, base, l);
  r[l] = (char)0xC2;
  r[l + 1] = (char)0xB7;
  r[l + 2] = 0;
  return r;
}

/* 找字符串内最后一个 ·（U+00B7，2 字节），无则返回 NULL。 */
static const char *strrstr_mid(const char *s) {
  const char *last = NULL;
  for (const char *p = s; *p; p++)
    if ((unsigned char)p[0] == 0xC2 && (unsigned char)p[1] == 0xB7)
      last = p;
  return last;
}

/* 解析 key 的父目录/成员名（对齐 durable split_index，取末段最后一个 ·）。父目录含尾分隔符。 */
static void shm_split_index(const char *key, char **parent, char **name,
                            bool *is_member) {
  *parent = NULL;
  *name = NULL;
  *is_member = false;
  const char *s = strrchr(key, '/');
  const char *last;
  size_t plen;
  if (!s || s == key) {
    plen = 1; /* 父前缀 "/" */
    last = s ? s + 1 : key;
  } else {
    plen = (size_t)(s - key) + 1; /* 含尾 '/' */
    last = s + 1;
  }
  const char *dot = strrstr_mid(last);
  if (dot && dot != last && dot[2]) {
    size_t prelen = (size_t)(dot - last);
    char *p = malloc(plen + prelen + 2 + 1);
    if (plen == 1 && (s == key)) {
      p[0] = '/';
      memcpy(p + 1, last, prelen + 2);
      p[plen + prelen + 2] = 0;
    } else {
      memcpy(p, key, plen);
      memcpy(p + plen, last, prelen + 2);
      p[plen + prelen + 2] = 0;
    }
    *parent = p;
    *name = strdup(dot + 2);
    *is_member = true;
  } else {
    char *p = malloc(plen + 1);
    if (plen == 1 && (s == key)) {
      p[0] = '/';
      p[1] = 0;
    } else {
      memcpy(p, key, plen);
      p[plen] = 0;
    }
    *parent = p;
    *name = strdup(last);
  }
}

/* 解析 index body（[4B count LE]name1\nname2...），返回 malloc 成员名数组。 */
static char **parse_index_body(const uint8_t *raw, int32_t raw_len, int32_t *oc) {
  *oc = 0;
  if (!raw || raw_len < 4)
    return NULL;
  const char *s = (const char *)raw + 4;
  int32_t slen = raw_len - 4;
  if (slen == 0)
    return NULL;
  char **names = malloc(sizeof(char *) * (size_t)(slen + 1));
  if (!names)
    return NULL;
  int32_t cnt = 0, start = 0;
  for (int32_t i = 0; i <= slen; i++) {
    if (i == slen || s[i] == '\n') {
      if (i > start)
        names[cnt++] = strndup(s + start, (size_t)(i - start));
      start = i + 1;
    }
  }
  *oc = cnt;
  return names;
}

/* 坐标段 → stringkeymap dims（对齐 durable grow_coord_dims，单成员）。 */
static void grow_coord_dims_one(const char *name, int32_t *dims, int32_t *ndim) {
  int64_t coords[8];
  int n = parse_coord(name, coords, 8);
  if (n < 0) {
    dims[0] = 1;
    *ndim = 1;
    return;
  }
  *ndim = n;
  for (int i = 0; i < n; i++)
    dims[i] = (int32_t)(coords[i] + 1);
}

/* 确保 memindex 存在（不存在 → 建空 index）。 */
static void ensure_memindex(kvspace_t *kv, const char *mem) {
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)mem,
                            (int)strlen(mem));
  if (h && h->has_value)
    return;
  uint8_t *iv;
  int32_t ivl = kvspaceXvalueNewIndex(NULL, 0, &iv);
  shm_set_raw(kv, mem, iv, ivl);
  free(iv);
}

/* 向 memindex 追加成员名（幂等）。 */
static int add_child_index(kvspace_t *kv, const char *mem, const char *name) {
  char **names = NULL;
  int32_t nnames = 0;
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)mem,
                            (int)strlen(mem));
  if (h && h->has_value) {
    uint8_t *raw;
    int32_t rl;
    if (read_tlv(kv, h->box_offset, &raw, &rl) == 0) {
      xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
      if (hh.ref == 0 && is_kind(&hh, KVSPACE_KIND_EXT_INDEX))
        return 0; /* extindex：成员由 extpath 展开，不维护本地 childs */
      if (hh.ref == 0 && is_kind(&hh, KVSPACE_KIND_INDEX))
        names = parse_index_body(hh.raw, hh.raw_len, &nnames);
    }
  }
  for (int32_t i = 0; i < nnames; i++)
    if (strcmp(names[i], name) == 0) {
      for (int32_t j = 0; j < nnames; j++)
        free(names[j]);
      free(names);
      return 0;
    }
  char **nn = realloc(names, sizeof(char *) * (size_t)(nnames + 1));
  if (!nn) {
    for (int32_t j = 0; j < nnames; j++)
      free(names[j]);
    free(names);
    return -1;
  }
  nn[nnames] = strdup(name);
  uint8_t *iv;
  int32_t ivl = kvspaceXvalueNewIndex((const char **)nn, nnames + 1, &iv);
  int rc = shm_set_raw(kv, mem, iv, ivl);
  free(iv);
  for (int32_t j = 0; j <= nnames; j++)
    free(nn[j]);
  free(nn);
  return rc;
}

/* 从 memindex 移除成员名（幂等）。 */
static int remove_child_index(kvspace_t *kv, const char *mem, const char *name) {
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)mem,
                            (int)strlen(mem));
  if (!h || !h->has_value)
    return 0;
  uint8_t *raw;
  int32_t rl;
  if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
    return 0;
  xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
  if (hh.ref != 0 || !is_kind(&hh, KVSPACE_KIND_INDEX))
    return 0;
  int32_t nnames;
  char **names = parse_index_body(hh.raw, hh.raw_len, &nnames);
  if (!names)
    return 0;
  int32_t j = 0;
  for (int32_t i = 0; i < nnames; i++) {
    if (strcmp(names[i], name) == 0) {
      free(names[i]);
      continue;
    }
    names[j++] = names[i];
  }
  if (j == nnames) {
    for (int32_t i = 0; i < j; i++)
      free(names[i]);
    free(names);
    return 0;
  }
  uint8_t *iv;
  int32_t ivl = kvspaceXvalueNewIndex((const char **)names, j, &iv);
  int rc = shm_set_raw(kv, mem, iv, ivl);
  free(iv);
  for (int32_t i = 0; i < j; i++)
    free(names[i]);
  free(names);
  return rc;
}

/* 写成员时沿父链逐层兜底容器值（leaf base + 中间层 object/stringkeymap）并注册成员（对齐 durable）。
 * parent 是尾 · 的成员父目录，name 是该成员名；逐层向上建容器值并注册成员到各自 memindex。 */
static void ensure_member_chain(kvspace_t *kv, char *parent, char *name) {
  char *dir = strdup(parent);
  char *child = strdup(name);
  for (;;) {
    char *base = strip_dir_suf_alloc(dir);
    art_hdr_t *ch = art_search(kv, kv->hdr->art_root, (const uint8_t *)base,
                               (int)strlen(base));
    if (!ch || !ch->has_value) {
      if (coord_is_coord(child)) {
        int32_t dims[8];
        int32_t ndim;
        grow_coord_dims_one(child, dims, &ndim);
        uint8_t *mv;
        int32_t mvl = kvspaceXvalueEncode(KVSPACE_KIND_MAP, NULL, 0, dims, ndim, &mv);
        shm_set_raw(kv, base, mv, mvl);
        free(mv);
      } else {
        uint8_t *ov;
        int32_t ovl = kvspaceXvalueEncode(KVSPACE_KIND_OBJ, NULL, 0, NULL, 0, &ov);
        shm_set_raw(kv, base, ov, ovl);
        free(ov);
      }
    }
    ensure_memindex(kv, dir);
    add_child_index(kv, dir, child);
    char *pp = NULL, *pn = NULL;
    bool pm = false;
    shm_split_index(base, &pp, &pn, &pm);
    free(base);
    if (!pm) {
      free(pp);
      free(pn);
      free(dir);
      free(child);
      break; /* 父是层级目录（如 /），shm 不维护根 index */
    }
    free(dir);
    dir = pp;
    free(child);
    child = pn;
  }
}

/* 非容器写的父索引维护：member → ensure_member_chain；dir index/extindex → 注册父 index。
   kvspaceShmSet 与零拷贝 kvspaceShmWriteNewPlace 共用，杜绝逻辑分叉。 */
static void shm_ensure_indexes(kvspace_t *kv, const char *kbuf,
                               const xvalue_head_t *hh) {
  char *parent = NULL, *name = NULL;
  bool is_member = false;
  shm_split_index(kbuf, &parent, &name, &is_member);
  if (is_member) {
    ensure_member_chain(kv, parent, name);
    free(parent);
    free(name);
    return;
  }
  free(parent);
  free(name);
  size_t l = strlen(kbuf);
  bool is_dir = (l > 0 && kbuf[l - 1] == '/') ||
                (l >= 2 && (unsigned char)kbuf[l - 2] == 0xC2 &&
                 (unsigned char)kbuf[l - 1] == 0xB7);
  if (is_dir && hh->ref == 0 &&
      (is_kind(hh, KVSPACE_KIND_INDEX) || is_kind(hh, KVSPACE_KIND_EXT_INDEX))) {
    char *strip = strip_dir_suf_alloc(kbuf);
    char *pp = NULL, *pn = NULL;
    bool pm = false;
    shm_split_index(strip, &pp, &pn, &pm);
    if (pn && pn[0]) {
      char *dn = pn;
      if (kbuf[l - 1] == '/') {
        size_t nl = strlen(pn);
        dn = malloc(nl + 2);
        memcpy(dn, pn, nl);
        dn[nl] = '/';
        dn[nl + 1] = 0;
      }
      add_child_index(kv, pp, dn);
      if (dn != pn)
        free(dn);
    }
    free(pp);
    free(pn);
    free(strip);
  }
}

/* 分配 box、就地写 head（kindexpr, body_len），art_ins 挂树，返回 body 偏移指针。
   已存在 key 先释放旧 box（新位置写=换 box）。零拷贝写路径唯一分配点。 */
static int shm_alloc_head(kvspace_t *kv, const char *key, const char *kindexpr,
                          int32_t headlen, int32_t body_len, uint8_t **body) {
  int32_t total = headlen + body_len;
  art_hdr_t *old = art_search(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key));
  if (old && old->has_value)
    sbo_free(kv->sbo_meta, old->box_offset);
  uint64_t off = sbo_alloc(kv->sbo_meta, (size_t)total);
  if (off == (uint64_t)-1)
    return -1;
  kvspaceXvalueWriteHead(kv->sbo_data + off, kindexpr, body_len);
  kv->hdr->art_root = art_ins(kv, kv->hdr->art_root, (const uint8_t *)key,
                              (int)strlen(key), 0, off);
  *body = kv->sbo_data + off + headlen;
  return 0;
}

int kvspaceShmSet(kvspace_t *kv, const char *key, const uint8_t *val,
                int32_t val_len) {
  if (!kv || !key)
    return -1;
  if (!val && val_len > 0)
    return -1;
  if (val_len <= 0) {
    /* None → 写 1 字节空 kind TLV（sbo 不支持 0 字节），读时 read_tlv 判 None 返 len 0。 */
    static const uint8_t none_tlv[1] = {0};
    val = none_tlv;
    val_len = 1;
  }
  char kbuf[1024];
  resolve_path(kv, key, kbuf, sizeof(kbuf)); // always resolve through link
  if (strstr(kbuf, "//"))
    return -1;

  xvalue_head_t hh = kvspaceXvalueDecodeHead(val, val_len);

  /* 目录 kind（index/extindex）必须落在目录键（尾 / 或 ·）。 */
  if (hh.ref == 0 && (is_kind(&hh, KVSPACE_KIND_INDEX) ||
                      is_kind(&hh, KVSPACE_KIND_EXT_INDEX))) {
    size_t l = strlen(kbuf);
    bool is_dir = (l > 0 && kbuf[l - 1] == '/') ||
                  (l >= 2 && (unsigned char)kbuf[l - 2] == 0xC2 &&
                   (unsigned char)kbuf[l - 1] == 0xB7);
    if (!is_dir)
      return -1;
  }

  /* 容器值（object/stringkeymap）：值写 p（无后缀，body 空），memindex p· 单独写。 */
  if (hh.ref == 0 && (is_kind(&hh, KVSPACE_KIND_OBJ) ||
                      is_kind(&hh, KVSPACE_KIND_MAP))) {
    char *base = strip_dir_suf_alloc(kbuf);
    if (!base || !base[0]) {
      free(base);
      return -1;
    }
    int32_t nnames;
    char **names = parse_index_body(hh.raw, hh.raw_len, &nnames);
    char *kind = strndup(hh.kind, hh.kind_len);
    uint8_t *cv;
    int32_t cvl = kvspaceXvalueEncodeMode(kind, NULL, 0, hh.dims, hh.ndim, 0,
                                          hh.ro, hh.vid, &cv);
    int rc = shm_set_raw(kv, base, cv, cvl);
    free(cv);
    free(kind);
    if (rc < 0) {
      if (names) {
        for (int32_t i = 0; i < nnames; i++)
          free(names[i]);
        free(names);
      }
      free(base);
      return -1;
    }
    char *mem = memjoin(base);
    uint8_t *iv;
    int32_t ivl = kvspaceXvalueNewIndex((const char **)names, nnames, &iv);
    rc = shm_set_raw(kv, mem, iv, ivl);
    free(iv);
    free(mem);
    /* 注册 base 为其父 memindex 成员（嵌套容器）。 */
    char *pp = NULL, *pn = NULL;
    bool pm = false;
    shm_split_index(base, &pp, &pn, &pm);
    if (pm)
      add_child_index(kv, pp, pn);
    free(pp);
    free(pn);
    if (names) {
      for (int32_t i = 0; i < nnames; i++)
        free(names[i]);
      free(names);
    }
    free(base);
    return rc;
  }

  /* 成员/目录索引维护（与 WriteNewPlace 共用），随后落盘。 */
  shm_ensure_indexes(kv, kbuf, &hh);
  return shm_set_raw(kv, kbuf, val, val_len);
}

int kvspaceShmWriteInPlace(kvspace_t *kv, const char *key, int resolve,
                           int32_t body_len, uint8_t **body) {
  if (!kv || !key || !body || body_len < 0)
    return -1;
  char kbuf[1024];
  if (resolve)
    resolve_path(kv, key, kbuf, sizeof(kbuf));
  else {
    strncpy(kbuf, key, sizeof(kbuf) - 1);
    kbuf[sizeof(kbuf) - 1] = '\0';
  }
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                            (int)strlen(kbuf));
  if (!h || !h->has_value)
    return -1;
  uint8_t *raw;
  int32_t rl;
  if (read_tlv(kv, h->box_offset, &raw, &rl) < 0 || rl <= 0)
    return -1; /* None 或读失败 → 强制走 NewPlace */
  xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
  if (hh.raw_len != body_len)
    return -1; /* 前置条件：同 body_len（同 kind 覆写） */
  *body = raw + kvspaceXvalueHeadLen(&hh);
  return 0;
}

int kvspaceShmWriteNewPlace(kvspace_t *kv, const char *key, const char *kindexpr,
                            int32_t body_len, uint8_t **body) {
  if (!kv || !key || !kindexpr || !body || body_len < 0)
    return -1;
  char kbuf[1024];
  resolve_path(kv, key, kbuf, sizeof(kbuf));
  if (strstr(kbuf, "//"))
    return -1;

  int32_t headlen = kvspaceXvalueHeadLenForKindexpr(kindexpr);
  uint8_t hbuf[512];
  if (headlen > (int32_t)sizeof(hbuf))
    return -1;
  kvspaceXvalueWriteHead(hbuf, kindexpr, 0);
  xvalue_head_t hh = kvspaceXvalueDecodeHead(hbuf, headlen);

  size_t l = strlen(kbuf);
  bool is_dir = (l > 0 && kbuf[l - 1] == '/') ||
                (l >= 2 && (unsigned char)kbuf[l - 2] == 0xC2 &&
                 (unsigned char)kbuf[l - 1] == 0xB7);
  if (hh.ref == 0 &&
      (is_kind(&hh, KVSPACE_KIND_INDEX) || is_kind(&hh, KVSPACE_KIND_EXT_INDEX)) &&
      !is_dir)
    return -1;

  /* 容器值（object/stringkeymap，body 恒空）：base 空 box + 空 memindex + 注册父。 */
  if (hh.ref == 0 &&
      (is_kind(&hh, KVSPACE_KIND_OBJ) || is_kind(&hh, KVSPACE_KIND_MAP))) {
    if (body_len != 0)
      return -1;
    char *base = strip_dir_suf_alloc(kbuf);
    if (!base || !base[0]) {
      free(base);
      return -1;
    }
    char *mem = memjoin(base);
    uint8_t *iv;
    int32_t ivl = kvspaceXvalueNewIndex(NULL, 0, &iv);
    if (ivl > 0) {
      shm_set_raw(kv, mem, iv, ivl);
      free(iv);
    }
    free(mem);
    char *pp = NULL, *pn = NULL;
    bool pm = false;
    shm_split_index(base, &pp, &pn, &pm);
    if (pm)
      add_child_index(kv, pp, pn);
    free(pp);
    free(pn);
    int rc = shm_alloc_head(kv, base, kindexpr, headlen, 0, body);
    free(base);
    return rc;
  }

  shm_ensure_indexes(kv, kbuf, &hh);
  return shm_alloc_head(kv, kbuf, kindexpr, headlen, body_len, body);
}

int kvspaceShmListLen(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                      int32_t *out_count) {
  char **names;
  int32_t count;
  if (kvspaceShmList(kv, prefix, ex, resolve, &names, &count) != 0) {
    *out_count = 0;
    return -1;
  }
  for (int32_t i = 0; i < count; i++)
    free(names[i]);
  free(names);
  *out_count = count;
  return 0;
}

int kvspaceShmDel(kvspace_t *kv, const char *key) {
  if (!kv || !key)
    return -1;
  char kbuf[1024];
  resolve_path(kv, key, kbuf, sizeof(kbuf)); // POSIX rm: resolve all
  /* memindex 成员删除 → 同步从 p· 的 index 移除。 */
  char *parent = NULL, *name = NULL;
  bool is_member = false;
  shm_split_index(kbuf, &parent, &name, &is_member);
  if (is_member)
    remove_child_index(kv, parent, name);
  free(parent);
  free(name);
  bool d = false;
  kv->hdr->art_root = art_del(kv, kv->hdr->art_root, (const uint8_t *)kbuf,
                              (int)strlen(kbuf), 0, &d);
  return d ? 0 : -1;
}

int kvspaceShmDeltree(kvspace_t *kv, const char *prefix) {
  if (!kv || !prefix)
    return -1;
  // if prefix itself is a link, only delete the link
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)prefix,
                            (int)strlen(prefix));
  if (h && h->has_value) {
    uint8_t *raw;
    int32_t rl;
    read_tlv(kv, h->box_offset, &raw, &rl);
    xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
    if (hh.ref == 1) {
      return kvspaceShmDel(kv, prefix);
    }
  }
  char *e = edir(prefix); // ensure trailing / for listing children
  char **ns;
  int32_t nc;
  kvspaceShmList(kv, e, false, 1, &ns, &nc);
  for (int i = 0; i < nc; i++) {
    char *c = pjoin(e, ns[i]);
    kvspaceShmDeltree(kv, c);
    free(c);
  }
  for (int i = 0; i < nc; i++)
    free(ns[i]);
  free(ns);

  /* 成员目录 marker（prefix·，U+00B7）：成员 key = prefix·<name>，直接拼接后递归删除。
     slash 版 e=prefix/ 覆盖不到 · 成员（json 的 object/stringkeymap 落盘形态）。 */
  size_t pl = strlen(prefix);
  char *m = malloc(pl + 3);
  memcpy(m, prefix, pl);
  m[pl] = (char)0xC2;
  m[pl + 1] = (char)0xB7;
  m[pl + 2] = 0;
  char **ms;
  int32_t mc;
  kvspaceShmList(kv, m, false, 1, &ms, &mc);
  for (int i = 0; i < mc; i++) {
    size_t ml = strlen(m), nl = strlen(ms[i]);
    char *c = malloc(ml + nl + 1);
    memcpy(c, m, ml);
    memcpy(c + ml, ms[i], nl + 1);
    kvspaceShmDeltree(kv, c);
    free(c);
  }
  for (int i = 0; i < mc; i++)
    free(ms[i]);
  free(ms);
  kvspaceShmDel(kv, m);
  free(m);

  kvspaceShmDel(kv, prefix);
  if (strcmp(e, prefix) != 0)
    kvspaceShmDel(kv, e);
  free(e);
  return 0;
}

/* 成员目录 marker（p·，U+00B7）。 */
static char *memdir(const char *p) {
  size_t l = strlen(p);
  char *r = malloc(l + 3);
  memcpy(r, p, l);
  r[l] = (char)0xC2;
  r[l + 1] = (char)0xB7;
  r[l + 2] = 0;
  return r;
}

/* 单 key 原样拷贝（Set 可能移动 slab，先拷出）。 */
int kvspaceShmCp(kvspace_t *kv, const char *src, const char *dst) {
  if (!kv || !src || !dst)
    return -1;
  int32_t rl;
  uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
  if (!raw || rl <= 0)
    return -1;
  uint8_t *tmp = malloc((size_t)rl);
  memcpy(tmp, raw, (size_t)rl);
  int rc = kvspaceShmSet(kv, dst, tmp, rl);
  free(tmp);
  return rc;
}

/* 递归拷贝：镜像 kvspaceShmDeltree 的遍历（/ 子节点 + · 成员），逐 key 原样复制。
   index 由 ART 前缀扫描派生，故写入 dst 各 key 即自动重建目录；extindex marker 一并复制。 */
static int cptree_rec(kvspace_t *kv, const char *src, const char *dst) {
  {
    int32_t rl;
    uint8_t *raw = kvspaceShmGet(kv, src, 0, &rl);
    if (raw && rl > 0) {
      uint8_t *tmp = malloc((size_t)rl);
      memcpy(tmp, raw, (size_t)rl);
      kvspaceShmSet(kv, dst, tmp, rl);
      free(tmp);
    }
  }
  char *es = edir(src), *ed = edir(dst);
  char **ns;
  int32_t nc;
  kvspaceShmList(kv, es, false, 1, &ns, &nc);
  for (int i = 0; i < nc; i++) {
    char *cs = pjoin(es, ns[i]), *cd = pjoin(ed, ns[i]);
    cptree_rec(kv, cs, cd);
    free(cs);
    free(cd);
  }
  for (int i = 0; i < nc; i++)
    free(ns[i]);
  free(ns);
  free(es);
  free(ed);

  char *ms = memdir(src), *md = memdir(dst);
  kvspaceShmCp(kv, ms, md); /* memindex marker 值（extindex marker / map dims）本身。 */
  char **mms;
  int32_t mc;
  kvspaceShmList(kv, ms, false, 1, &mms, &mc);
  for (int i = 0; i < mc; i++) {
    size_t msl = strlen(ms), mdl = strlen(md), nl = strlen(mms[i]);
    char *cs = malloc(msl + nl + 1);
    memcpy(cs, ms, msl);
    memcpy(cs + msl, mms[i], nl + 1);
    char *cd = malloc(mdl + nl + 1);
    memcpy(cd, md, mdl);
    memcpy(cd + mdl, mms[i], nl + 1);
    cptree_rec(kv, cs, cd);
    free(cs);
    free(cd);
  }
  for (int i = 0; i < mc; i++)
    free(mms[i]);
  free(mms);
  free(ms);
  free(md);
  return 0;
}

int kvspaceShmCptree(kvspace_t *kv, const char *src, const char *dst) {
  if (!kv || !src || !dst)
    return -1;
  kvspaceShmDeltree(kv, dst); /* 覆盖语义：先清 dst 子树。 */
  return cptree_rec(kv, src, dst);
}

int kvspaceShmMkindex(kvspace_t *kv, const char *path) {
  if (!kv || !path)
    return -1;
  char *d = edir(path);
  uint8_t *v;
  int32_t vl = kvspaceXvalueNewIndex(NULL, 0, &v);
  int r = kvspaceShmSet(kv, d, v, vl);
  free(v);
  free(d);
  return r;
}

/* ── stringkeymap 坐标段 [s0,s1,...] 解析与排序（对齐 kvspace-durable coord） ── */

/* 坐标段结构判定：任意非空 [..]，禁止嵌套 [ ]。小数/字符串坐标（[12.24,x]）也算坐标段。 */
static int coord_is_coord(const char *name) {
  if (!name || name[0] != '[')
    return 0;
  size_t n = strlen(name);
  if (n < 3 || name[n - 1] != ']')
    return 0;
  for (size_t i = 1; i + 1 < n; i++)
    if (name[i] == '[' || name[i] == ']')
      return 0;
  return 1;
}

/* 解析整数坐标段，成功填充 coords 并返回维数；非整数返回 -1。 */
static int parse_coord(const char *name, int64_t *coords, int maxn) {
  if (!name || name[0] != '[')
    return -1;
  int n = 0;
  int64_t cur = 0;
  bool has = false;
  for (const char *p = name + 1; *p; p++) {
    if (*p >= '0' && *p <= '9') {
      cur = cur * 10 + (*p - '0');
      has = true;
    } else if (*p == ',') {
      if (!has || n >= maxn)
        return -1;
      coords[n++] = cur;
      cur = 0;
      has = false;
    } else if (*p == ']') {
      if (!has || n >= maxn)
        return -1;
      coords[n++] = cur;
      return (p[1] == '\0') ? n : -1;
    } else {
      return -1;
    }
  }
  return -1;
}

static int coord_cmp(const char *a, const char *b) {
  int ia = coord_is_coord(a), ib = coord_is_coord(b);
  if (!ia && !ib)
    return strcmp(a, b);
  if (!ia)
    return 1; /* 非坐标段排后 */
  if (!ib)
    return -1;
  int64_t ca[8], cb[8];
  int na = parse_coord(a, ca, 8);
  int nb = parse_coord(b, cb, 8);
  if (na < 0 || nb < 0)
    return strcmp(a, b); /* 含小数/字符串坐标 → 字典序 */
  for (int i = 0; i < na && i < nb; i++) {
    if (ca[i] != cb[i])
      return ca[i] < cb[i] ? -1 : 1;
  }
  if (na != nb)
    return na < nb ? -1 : 1;
  return 0;
}

static int qsort_coord_cmp(const void *a, const void *b) {
  return coord_cmp(*(const char *const *)a, *(const char *const *)b);
}

/* 读 memindex（p·）的成员名：index body（[4B count LE]name1\nname2...）是成员名唯一权威。
 * 仅 p·（尾 ·）目录命中；slash 目录（p/）不自动维护 index，调用方回退 ART scan。
 * stringkeymap：读容器值 p 的 kind 判定后按坐标 row-major 数值升序（对齐 durable coord.cmp_coord）。 */
static int read_index_names(kvspace_t *kv, const char *dir, char ***on, int32_t *oc) {
  *on = NULL;
  *oc = 0;
  size_t dl = strlen(dir);
  if (dl < 2 || !((unsigned char)dir[dl - 2] == 0xC2 && (unsigned char)dir[dl - 1] == 0xB7))
    return 0; /* 仅 memindex（p·） */
  art_hdr_t *h = art_search(kv, kv->hdr->art_root, (const uint8_t *)dir, (int)dl);
  if (!h || !h->has_value)
    return 0;
  uint8_t *raw;
  int32_t rl;
  if (read_tlv(kv, h->box_offset, &raw, &rl) < 0)
    return 0;
  xvalue_head_t hh = kvspaceXvalueDecodeHead(raw, rl);
  if (hh.ref != 0 || !is_kind(&hh, KVSPACE_KIND_INDEX))
    return 0;
  char **names = parse_index_body(hh.raw, hh.raw_len, oc);
  if (!names)
    return 1;
  /* stringkeymap：容器值 p（strip · 后无后缀）的 kind 决定 row-major 升序。 */
  char *base = strip_dir_suf_alloc(dir);
  bool is_map = false;
  art_hdr_t *ch = art_search(kv, kv->hdr->art_root, (const uint8_t *)base, (int)strlen(base));
  if (ch && ch->has_value) {
    uint8_t *craw;
    int32_t crl;
    if (read_tlv(kv, ch->box_offset, &craw, &crl) == 0) {
      xvalue_head_t chh = kvspaceXvalueDecodeHead(craw, crl);
      is_map = chh.ref == 0 && is_kind(&chh, KVSPACE_KIND_MAP);
    }
  }
  free(base);
  if (is_map && *oc > 1)
    qsort(names, (size_t)*oc, sizeof(char *), qsort_coord_cmp);
  *on = names;
  return 1;
}

/* 提取直接成员名长度：到第一个 / 或 ·（U+00B7，2 字节）为止。 */
static int child_name_len(const char *rest, int restlen) {
  for (int i = 0; i < restlen; i++) {
    if (rest[i] == '/')
      return i;
    if (i + 1 < restlen && (unsigned char)rest[i] == 0xC2 &&
        (unsigned char)rest[i + 1] == 0xB7)
      return i;
  }
  return restlen;
}

int kvspaceShmList(kvspace_t *kv, const char *prefix, bool ex, int resolve,
                 char ***on, int32_t *oc) {
  if (!kv || !prefix || !on || !oc)
    return -1;
  *on = NULL;
  *oc = 0;
  if (bad_dir_prefix(prefix))
    return -1;
  const char *pfx = prefix;
  char tbuf[1024];
  if (resolve) {
    resolve_path(kv, prefix, tbuf, sizeof(tbuf));
    pfx = tbuf;
  }
  /* memindex（p·）：读 index body 成员名（唯一权威）；stringkeymap 按坐标 row-major 升序。 */
  if (read_index_names(kv, pfx, on, oc))
    return 0;
  int plen = (int)strlen(pfx);
  char **out = malloc(sizeof(char *) * 4096);
  int32_t n = 0;
  char buf[2048];
  memset(buf, 0, sizeof(buf));
  if (kv->hdr->art_root < 0)
    return 0;
  art_scan(kv, kv->hdr->art_root, buf, 0, (int)sizeof(buf), pfx, plen, &out,
           &n);
  // filter: only direct children (one level below prefix)
  char **filt = malloc(sizeof(char *) * n);
  int32_t fn = 0;
  for (int i = 0; i < n; i++) {
    const char *k = out[i];
    int kl = (int)strlen(k);
    if (kl <= plen)
      continue;
    // extract the name segment immediately after prefix
    const char *rest = k + plen;
    int nlen = child_name_len(rest, kl - plen);
    if (nlen == 0)
      continue;
    // dedup
    bool dup = false;
    for (int j = 0; j < fn; j++)
      if (strncmp(filt[j], rest, nlen) == 0 && filt[j][nlen] == '\0') {
        dup = true;
        break;
      }
    if (!dup) {
      filt[fn] = strndup(rest, nlen);
      fn++;
    }
  }
  for (int i = 0; i < n; i++)
    free(out[i]);
  free(out);

  /* extindex 展开：ex 且 prefix 是 extindex → 追加 extpath 的直接子项。 */
  if (ex) {
    char extpath[1024];
    char *d = edir(pfx);
    if (dir_ext_path(kv, d, extpath, sizeof extpath)) {
      char **eo = malloc(sizeof(char *) * 4096);
      int32_t en = 0;
      char ebuf[2048];
      memset(ebuf, 0, sizeof ebuf);
      int el = (int)strlen(extpath);
      art_scan(kv, kv->hdr->art_root, ebuf, 0, (int)sizeof ebuf, extpath, el, &eo, &en);
      filt = realloc(filt, sizeof(char *) * (size_t)(fn + en));
      for (int i = 0; i < en; i++) {
        const char *k = eo[i];
        int kl = (int)strlen(k);
        if (kl <= el)
          continue;
        const char *rest = k + el;
        int nlen = child_name_len(rest, kl - el);
        if (nlen == 0)
          continue;
        bool dup = false;
        for (int j = 0; j < fn; j++)
          if (strncmp(filt[j], rest, (size_t)nlen) == 0 && filt[j][nlen] == '\0') {
            dup = true;
            break;
          }
        if (!dup) {
          filt[fn] = strndup(rest, (size_t)nlen);
          fn++;
        }
      }
      for (int i = 0; i < en; i++)
        free(eo[i]);
      free(eo);
    }
    free(d);
  }

  *on = filt;
  *oc = fn;
  return 0;
}

int kvspaceShmExtindex(kvspace_t *kv, const char *p, const char *ep) {
  uint8_t *v;
  int32_t vl = kvspaceXvalueNewExtindex(ep, NULL, 0, &v);
  int r = kvspaceShmSet(kv, p, v, vl);
  free(v);
  return r;
}
int kvspaceShmDelextindex(kvspace_t *kv, const char *p) { return kvspaceShmDel(kv, p); }

/* ============ Watch/Notify ============ */
static int wslot(const char *k) {
  uint32_t h = 5381;
  for (const char *p = k; *p; p++)
    h = ((h << 5) + h) + (uint8_t)*p;
  return (int)(h % WATCH_TABLE_SZ);
}
int kvspaceShmNotify(kvspace_t *kv, const char *k, const uint8_t *v, int32_t vl) {
  if (!kv || !k)
    return -1;
  int s = wslot(k);
  pthread_mutex_lock(&kv->wlock);
  watch_t *w = &kv->watches[s];
  if (w->key[0] && strcmp(w->key, k) == 0) {
    pthread_mutex_lock(&w->mtx);
    free(w->val);
    w->val = v ? memcpy(malloc(vl), v, vl) : NULL;
    w->val_len = vl;
    w->ready = true;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->mtx);
  }
  pthread_mutex_unlock(&kv->wlock);
  return 0;
}
uint8_t *kvspaceShmWatch(kvspace_t *kv, const char *k, int32_t to, int32_t *ol) {
  if (!kv || !k || !ol)
    return NULL;
  *ol = 0;
  int s = wslot(k);
  pthread_mutex_lock(&kv->wlock);
  watch_t *w = &kv->watches[s];
  strncpy(w->key, k, 255);
  w->key[255] = '\0';
  w->ready = false;
  free(w->val);
  w->val = NULL;
  pthread_mutex_unlock(&kv->wlock);
  pthread_mutex_lock(&w->mtx);
  if (!w->ready) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += to / 1000;
    ts.tv_nsec += (to % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000L;
    }
    pthread_cond_timedwait(&w->cond, &w->mtx, &ts);
  }
  uint8_t *r = NULL;
  if (w->ready && w->val) {
    r = w->val;
    *ol = w->val_len;
    w->val = NULL;
  }
  w->ready = false;
  pthread_mutex_unlock(&w->mtx);
  return r;
}
