#!/usr/bin/env python3
"""make_p2_abi.py — 给 kvspace 规范前端补 P2 token ABI（公共 ABI 测量用）。

改：
  include/kvspace/kvspace.h  : kvspaceViewToken_t + 7 个原型
  src/frontend.c             : vtable 字段 + 可选 dlsym + trampolines（后端缺失 → -2）
产物目录：/tmp/cycle-fastpath/p2/frontend/kvspace
"""
import shutil
import sys
from pathlib import Path

SRC = Path("/tmp/cycle-audit16/src/kvspace")
OUT = Path("/tmp/cycle-fastpath/p2/frontend/kvspace")

HDR = '''
/* ── P2: 版本化 view token（前缀/leaf 缓存载体）─────────────────── */
typedef struct {
    uint8_t *head;        /* XValue 起点（shm 映射内，借用） */
    uint8_t *body;        /* head + headlen */
    uint32_t headlen;
    uint32_t value_len;
    uint64_t cap;         /* 整 box 物理容量 */
    uint64_t body_cap;
    uint32_t struct_gen;  /* 结构版本：子集变化/搬迁/重分配才变 */
    uint32_t value_gen;   /* 值版本：每次值写/删递增（seqlock） */
    uint64_t store_id;    /* 句柄身份：close/重开后旧 token 失效 */
    uint32_t node_id;
    uint32_t _pad;
} kvspaceViewToken_t;

int kvspaceViewRelease(void *h, kvspaceViewToken_t *tok);
int kvspaceViewSweep(void *h);
int kvspaceViewResolvePrefix(void *h, const char *path, kvspaceRef_t *ref);
int kvspaceViewResolve(void *h, const char *key, kvspaceViewToken_t *tok);
int kvspaceViewFromRef(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                       kvspaceViewToken_t *tok);
int kvspaceViewResolveFrom(void *h, const kvspaceRef_t *parent, const char *rel,
                           kvspaceViewToken_t *tok);
int kvspaceViewCheck(void *h, const kvspaceViewToken_t *tok);
int kvspaceViewWrite(void *h, kvspaceViewToken_t *tok, const uint8_t *data, uint32_t len);
int kvspaceViewDel(void *h, kvspaceViewToken_t *tok, const char *key);
int kvspaceViewRefresh(void *h, kvspaceViewToken_t *tok, const char *key);
'''

VT_FIELDS = '''    int  (*viewrelease)(void *h, kvspaceViewToken_t *tok);
    int  (*viewsweep)(void *h);
    int  (*viewresolveprefix)(void *h, const char *path, kvspaceRef_t *ref);
    int  (*viewresolve)(void *h, const char *key, kvspaceViewToken_t *tok);
    int  (*viewfromref)(void *h, const kvspaceRef_t *ref, const char *key_fallback,
                        kvspaceViewToken_t *tok);
    int  (*viewresolvefrom)(void *h, const kvspaceRef_t *parent, const char *rel,
                            kvspaceViewToken_t *tok);
    int  (*viewcheck)(void *h, const kvspaceViewToken_t *tok);
    int  (*viewwrite)(void *h, kvspaceViewToken_t *tok, const uint8_t *data, uint32_t len);
    int  (*viewdel)(void *h, kvspaceViewToken_t *tok, const char *key);
    int  (*viewrefresh)(void *h, kvspaceViewToken_t *tok, const char *key);
'''

TRAMP = '''
/* ── P2 token（后端缺符号 → -2，调用方回退）────────────────────── */
#define P2VT(field) (H(h)->vt->field)
int kvspaceViewRelease(void *h, kvspaceViewToken_t *tok) {
    if (!P2VT(viewrelease)) return -2;
    return P2VT(viewrelease)(H(h)->backend, tok);
}
int kvspaceViewSweep(void *h) {
    if (!P2VT(viewsweep)) return -2;
    return P2VT(viewsweep)(H(h)->backend);
}
int kvspaceViewResolvePrefix(void *h, const char *path, kvspaceRef_t *ref) {
    if (!P2VT(viewresolveprefix)) return -2;
    return P2VT(viewresolveprefix)(H(h)->backend, path, ref);
}
int kvspaceViewResolve(void *h, const char *key, kvspaceViewToken_t *tok) {
    if (!P2VT(viewresolve)) return -2;
    return P2VT(viewresolve)(H(h)->backend, key, tok);
}
int kvspaceViewFromRef(void *h, const kvspaceRef_t *ref, const char *k,
                       kvspaceViewToken_t *tok) {
    if (!P2VT(viewfromref)) return -2;
    return P2VT(viewfromref)(H(h)->backend, ref, k, tok);
}
int kvspaceViewResolveFrom(void *h, const kvspaceRef_t *parent, const char *rel,
                           kvspaceViewToken_t *tok) {
    if (!P2VT(viewresolvefrom)) return -2;
    return P2VT(viewresolvefrom)(H(h)->backend, parent, rel, tok);
}
int kvspaceViewCheck(void *h, const kvspaceViewToken_t *tok) {
    if (!P2VT(viewcheck)) return -2;
    return P2VT(viewcheck)(H(h)->backend, tok);
}
int kvspaceViewWrite(void *h, kvspaceViewToken_t *tok, const uint8_t *d, uint32_t n) {
    if (!P2VT(viewwrite)) return -2;
    return P2VT(viewwrite)(H(h)->backend, tok, d, n);
}
int kvspaceViewDel(void *h, kvspaceViewToken_t *tok, const char *key) {
    if (!P2VT(viewdel)) return -2;
    return P2VT(viewdel)(H(h)->backend, tok, key);
}
int kvspaceViewRefresh(void *h, kvspaceViewToken_t *tok, const char *key) {
    if (!P2VT(viewrefresh)) return -2;
    return P2VT(viewrefresh)(H(h)->backend, tok, key);
}
'''


def main():
    if not (SRC / "src" / "frontend.c").exists():
        sys.exit(f"missing {SRC}")
    if OUT.exists():
        shutil.rmtree(OUT)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(SRC, OUT, symlinks=True)

    h = OUT / "include" / "kvspace" / "kvspace.h"
    s = h.read_text()
    anchor = "int kvspaceSlotView(void *h, const kvspaceRef_t *ref,\n                    const char *key_fallback, kvspaceSlotView_t *out);"
    if anchor not in s:
        sys.exit("header anchor missing")
    h.write_text(s.replace(anchor, anchor + "\n" + HDR))

    f = OUT / "src" / "frontend.c"
    s = f.read_text()
    a2 = "    int  (*slotview)(void *h, const kvspaceRef_t *ref, const char *key_fallback,\n                     kvspaceSlotView_t *out);\n} kvspace_vt;"
    if a2 not in s:
        sys.exit("vt anchor missing")
    s = s.replace(a2, "    int  (*slotview)(void *h, const kvspaceRef_t *ref, const char *key_fallback,\n                     kvspaceSlotView_t *out);\n" + VT_FIELDS + "} kvspace_vt;")
    a3 = '    *(void **)&vt->setvaluebyref = dlsym(dl, "kvspaceSetValueByRef");'
    if a3 not in s:
        sys.exit("dlsym anchor missing")
    s = s.replace(a3, a3 + '''
    /* P2 可选符号：缺失时 trampoline 返回 -2 */
    *(void **)&vt->viewrelease = dlsym(dl, "kvspaceViewRelease");
    *(void **)&vt->viewsweep = dlsym(dl, "kvspaceViewSweep");
    *(void **)&vt->viewresolveprefix = dlsym(dl, "kvspaceViewResolvePrefix");
    *(void **)&vt->viewresolve = dlsym(dl, "kvspaceViewResolve");
    *(void **)&vt->viewfromref = dlsym(dl, "kvspaceViewFromRef");
    *(void **)&vt->viewresolvefrom = dlsym(dl, "kvspaceViewResolveFrom");
    *(void **)&vt->viewcheck = dlsym(dl, "kvspaceViewCheck");
    *(void **)&vt->viewwrite = dlsym(dl, "kvspaceViewWrite");
    *(void **)&vt->viewdel = dlsym(dl, "kvspaceViewDel");
    *(void **)&vt->viewrefresh = dlsym(dl, "kvspaceViewRefresh");''')
    s = s.rstrip() + "\n" + TRAMP
    f.write_text(s)
    print(f"patched {OUT}")


if __name__ == "__main__":
    main()
