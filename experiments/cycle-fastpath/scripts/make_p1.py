#!/usr/bin/env python3
"""make_p1.py — 生成 P1 累积变体源码（含前缀探针，便于同口径测 prefix+rel）。

p1a: strnlen+memcpy（去掉 1023B 定长拷贝）
p1b: p1a + 读路径免 sbo_allocated_size（用数据区上界）
p1c: p1b + 读路径跳过 xh_validate（写入仍全量校验；head-only 解码 + 边界检查）

变体目录：/tmp/cycle-fastpath/p1/<variant>/kvspace-c
"""
import shutil
import sys
from pathlib import Path

SRC = Path("/tmp/cycle-audit/src/kvspace-c")
OUT = Path("/tmp/cycle-fastpath/p1")
sys.path.insert(0, str(Path(__file__).resolve().parent))
from make_probe import PROBE  # noqa: E402


def sub(s, old, new, n=1):
    if s.count(old) != n:
        sys.exit(f"anchor mismatch ({s.count(old)} != {n}): {old[:80]}")
    return s.replace(old, new)


def func_span(s, marker):
    start = s.index(marker)
    end = s.index("\n}\n", start)
    return start, end


def patch_copy(p: Path):
    s = p.read_text()
    # Get / WriteInPlace（8 空格缩进的 else 分支）
    for marker, fail in (("uint8_t *kvspaceShmGet(", "NULL"),
                         ("int kvspaceShmWriteInPlace(", "-1")):
        a, b = func_span(s, marker)
        body = s[a:b]
        old = ("        strncpy(kbuf, key, sizeof(kbuf) - 1);\n"
               "        kbuf[sizeof(kbuf) - 1] = '\\0';")
        new = (f"        if (strlen(key) >= sizeof kbuf) return {fail};\n"
               "        memcpy(kbuf, key, strlen(key) + 1);")
        if body.count(old) != 1:
            sys.exit(f"copy anchor in {marker}: {body.count(old)}")
        s = s[:a] + body.replace(old, new) + s[b:]
    # Del（4 空格）
    a, b = func_span(s, "int kvspaceShmDel(")
    body = s[a:b]
    old = ("    strncpy(kbuf, key, sizeof(kbuf) - 1);\n"
           "    kbuf[sizeof(kbuf) - 1] = '\\0';")
    new = ("    if (strlen(key) >= sizeof kbuf) return -1;\n"
           "    memcpy(kbuf, key, strlen(key) + 1);")
    if body.count(old) != 1:
        sys.exit(f"copy anchor in Del: {body.count(old)}")
    s = s[:a] + body.replace(old, new) + s[b:]
    # resolve_fetch / resolve_path（strncpy(out, path, osz-1)）
    for marker, fail in (("static art_hdr_t *resolve_fetch(", "return NULL;"),
                         ("static void resolve_path(", "return;")):
        a, b = func_span(s, marker)
        body = s[a:b]
        old = ("    strncpy(out, path, osz - 1);\n    out[osz - 1] = '\\0';")
        new = (f"    if (strlen(path) >= (size_t)osz) {fail}\n"
               "    memcpy(out, path, strlen(path) + 1);")
        if body.count(old) != 1:
            sys.exit(f"copy anchor in {marker}: {body.count(old)}")
        s = s[:a] + body.replace(old, new) + s[b:]
    p.write_text(s)


def patch_nosize(p: Path):
    s = p.read_text()
    for old, new in (
        ("size_t sz = sbo_allocated_size(kv->sbo_meta, off);",
         "size_t sz = kv->hdr->sbo_data_size - off; /* P1: 免 shard 锁 */"),
        ("size_t sz = sbo_allocated_size(kv->sbo_meta, h->box_offset);",
         "size_t sz = kv->hdr->sbo_data_size - h->box_offset; /* P1 */"),
    ):
        s = sub(s, old, new)
    p.write_text(s)


def patch_nocheck(p: Path):
    hdr_h = p.parent / "xvalue_head.h"
    hdr_c = p.parent / "xvalue_head.c"
    # 声明 + 实现 head-only 视图
    s = hdr_h.read_text()
    s = sub(s, "int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out);",
            "int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out);\n"
            "/* P1：只解析头部，不做 body 全量校验（写入路径已校验）。 */\n"
            "int kvspaceXhHeadView(const uint8_t *data, uint64_t len, kvspaceXh *out);")
    hdr_h.write_text(s)
    c = hdr_c.read_text()
    c = sub(c, "int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out) {",
            "int kvspaceXhHeadView(const uint8_t *data, uint64_t len, kvspaceXh *out) {\n"
            "    return xh_header(data, len, out);\n"
            "}\n\n"
            "int kvspaceXhView(const uint8_t *data, uint64_t len, kvspaceXh *out) {")
    hdr_c.write_text(c)
    # 读路径改用 head-only + 上界检查
    s = p.read_text()
    s = sub(s, """    kvspaceXh wire;
    if (kvspaceXhView(s, sz, &wire) == 0) {
        if (wire.total > INT32_MAX)
            return -1;""", """    kvspaceXh wire;
    if (kvspaceXhHeadView(s, sz, &wire) == 0 && wire.total <= sz) {
        if (wire.total > INT32_MAX)
            return -1;""")
    s = sub(s, """        kvspaceXh wire;
        if (kvspaceXhView(s, sz, &wire) == 0) {""", """        kvspaceXh wire;
        if (kvspaceXhHeadView(s, sz, &wire) == 0 && wire.total <= sz) {""")
    p.write_text(s)


def main():
    if not (SRC / "src" / "kvspace.c").exists():
        sys.exit(f"missing {SRC}")
    OUT.mkdir(parents=True, exist_ok=True)
    steps = [("p1a", [patch_copy]),
             ("p1b", [patch_copy, patch_nosize]),
             ("p1c", [patch_copy, patch_nosize, patch_nocheck])]
    for name, fns in steps:
        d = OUT / name / "kvspace-c"
        if d.exists():
            shutil.rmtree(d)
        d.parent.mkdir(parents=True, exist_ok=True)
        shutil.copytree(SRC, d, symlinks=True)
        p = d / "src" / "kvspace.c"
        for fn in fns:
            fn(p)
        p.write_text(p.read_text() + PROBE)
        print(f"built {name}")


if __name__ == "__main__":
    main()
