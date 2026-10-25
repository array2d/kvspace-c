#!/usr/bin/env python3
"""make_probe.py — 生成 fastpath 前缀探针（测试构建，不改生产 ABI）。

从 /tmp/cycle-audit/src/kvspace-c（pinned v0.3.14）复制到 /tmp/cycle-fastpath/probe，
追加两个导出入口：
  kvspaceProbeFindDir(h, path)   → 目录节点 id（不要求有值）
  kvspaceProbeGetRel(h, node, rel, &len) → 从前缀节点走 rel（无前导 '/'）并借用读
"""
import shutil
import sys
from pathlib import Path

SRC = Path("/tmp/cycle-audit/src/kvspace-c")
OUT = Path("/tmp/cycle-fastpath/probe/src/kvspace-c")

PROBE = r'''

/* ── P0 PREFIX PROBE（仅测试构建）────────────────────────────── */
/* 走完 path 的最后一个字节，返回所在节点；*skip 写出该节点压缩前缀里已被
 * path 覆盖的字节数（ART 前缀压缩可能让 path 边界落在节点内部）。 */
static int32_t probe_walk_dir(kvspace_t *kv, int32_t nid, const char *key, int klen,
                              int32_t *skip) {
    int d = 0;
    while (nid >= 0) {
        art_hdr_t *h = art_hdr(kv, nid);
        if (!h || h->type == ART_MOVED)
            return -1;
        int in_prefix = 0;
        if (h->prefix_len) {
            int s = pfx_shared(h->prefix, h->prefix_len,
                               (const uint8_t *)key + d, klen - d);
            in_prefix = s;
            if (s != h->prefix_len && d + s != klen)
                return -1;
            d += s;
            if (d == klen) {
                if (skip) *skip = s;
                return nid;
            }
        }
        nid = art_child(kv, h, (uint8_t)key[d]);
        d++;
    }
    return -1;
}

int32_t kvspaceProbeFindDir(void *h, const char *path, int32_t *skip) {
    kvspace_t *kv = (kvspace_t *)h;
    if (skip) *skip = 0;
    if (!kv || !path)
        return -1;
    return probe_walk_dir(kv, kv->hdr->art_root, path, (int)strlen(path), skip);
}

uint8_t *kvspaceProbeGetRel(void *h, int32_t node, int32_t skip,
                           const char *rel, int32_t *out_len) {
    kvspace_t *kv = (kvspace_t *)h;
    if (out_len)
        *out_len = 0;
    if (!kv || node < 0 || !rel)
        return NULL;
    art_hdr_t *hd = art_hdr(kv, node);
    if (!hd)
        return NULL;
    const uint8_t *p = (const uint8_t *)rel;
    int rem = (int)strlen(rel);
    /* 前缀边界落在节点压缩前缀内部时，先把该节点前缀剩余部分对掉。 */
    while (skip < (int32_t)hd->prefix_len && rem > 0) {
        if (p[0] != hd->prefix[skip])
            return NULL;
        p++;
        rem--;
        skip++;
    }
    if (rem == 0) {
        uint8_t *raw;
        int32_t rl;
        if (!hd->has_value || read_tlv(kv, hd->box_offset, &raw, &rl) < 0)
            return NULL;
        *out_len = rl;
        return raw;
    }
    int32_t cid = art_child(kv, hd, p[0]);
    int32_t id = art_walk(kv, cid, p, rem, 1, NULL, NULL);
    if (id < 0)
        return NULL;
    art_hdr_t *lh = art_hdr(kv, id);
    if (!lh || !lh->has_value)
        return NULL;
    uint8_t *raw;
    int32_t rl;
    if (read_tlv(kv, lh->box_offset, &raw, &rl) < 0)
        return NULL;
    *out_len = rl;
    return raw;
}
'''


def main():
    if not (SRC / "src" / "kvspace.c").exists():
        sys.exit(f"pinned source missing: {SRC}")
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(SRC, OUT, symlinks=True)
    p = OUT / "src" / "kvspace.c"
    p.write_text(p.read_text() + PROBE)
    print(f"patched {p}")


if __name__ == "__main__":
    main()
