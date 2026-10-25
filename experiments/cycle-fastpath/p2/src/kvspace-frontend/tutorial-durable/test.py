#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""运行 tutorial/*.sh，对比脚本头部注释中的 expected 输出；
交叉校验 kvspace-c 与 kvspace-durable 的 head 编解码（rw/vid）字节一致；
并回归写侧落盘时机：下一次 op 落盘与退出兜底（issue #23）。"""

import ctypes
import os
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # kvspace/
WS = ROOT.parent  # array2d/
DURABLE_SO = WS / "kvspace-durable" / "target" / "release" / "libkvspace_durable.so"
KVSPACE_C_DIR = WS / "kvspace-c"
KVSPACE_C_SO = KVSPACE_C_DIR / "build" / "libkvspace-c.so"
FRONTEND_SO = os.environ.get("KVSPACE_SO", "/usr/lib/kvspace/libkvspace.so.1")

def extract_expected(script):
    """从脚本头部 # expected: ... # /end 提取预期输出行。"""
    lines = []
    in_block = False
    with open(script) as f:
        for raw in f:
            line = raw.rstrip('\n')
            if in_block:
                if line == '# /end':
                    break
                if line.startswith('# '):
                    lines.append(line[2:])
                elif line == '#':
                    lines.append('')
            elif line == '# expected:':
                in_block = True
    return lines

def run_script(script):
    """先 clear，再执行脚本，返回 stdout 行列表。"""
    kvbin = os.path.expanduser('~/.local/bin/kvspace')
    env = os.environ.copy()
    env.setdefault('KVSPACE', 'redis://127.0.0.1:6379')
    subprocess.run([kvbin, 'clear'], capture_output=True, timeout=10, env=env)
    r = subprocess.run(['bash', script], capture_output=True, text=True, timeout=30, env=env)
    return r.stdout.rstrip('\n').split('\n') if r.stdout.strip() else []

def test_script(script):
    expected = extract_expected(script)
    actual = run_script(script)
    if expected == actual:
        print(f'PASS  {os.path.basename(script)}')
        return True
    print(f'FAIL  {os.path.basename(script)}')
    print(f'  expected ({len(expected)} lines): {expected[:3]}...' if len(expected) > 3 else f'  expected: {expected}')
    print(f'  actual   ({len(actual)} lines):   {actual[:3]}...' if len(actual) > 3 else f'  actual:   {actual}')
    return False


# ── kvspace-c ↔ kvspace-durable 交叉校验（ctypes，字节级对齐） ──────────────

class HeadV(ctypes.Structure):
    # kvspaceHead_t 的**解码视图**（对齐 kvspace/include/kvspace/kvspace.h）。
    # 注意：wire 上没有独立的 ref / storetype 字节——它们是 flags 字节解码后的派生视图
    #（低 2 位 storage class、bit 2 指针位），此处字段仅为跨边界读取元数据方便。
    _fields_ = [
        ("headlen", ctypes.c_uint16),
        ("ref", ctypes.c_uint8),
        ("storetype", ctypes.c_uint8),
        ("ro", ctypes.c_uint8),
        ("vid", ctypes.c_uint32),
        ("body_len", ctypes.c_int32),
        ("ndim", ctypes.c_int32),
        ("dims", ctypes.c_int32 * 8),
        ("langtype", ctypes.c_uint8 * 256),
        ("langtype_len", ctypes.c_int32),
        ("body_offset", ctypes.c_int32),
    ]


U8P = ctypes.POINTER(ctypes.c_uint8)
I32P = ctypes.POINTER(ctypes.c_int32)
U32P = ctypes.POINTER(ctypes.c_uint32)
U8PP = ctypes.POINTER(U8P)

# codec 产出为 frontend malloc 缓冲，拷出后以 libc free 释放（无 kvspaceBytesFree）。
_libc = ctypes.CDLL(None); _libc.free.argtypes = [ctypes.c_void_p]


def setup_lib(lib):
    lib.kvspaceXvalueEncodeMode.argtypes = [ctypes.c_char_p, U8P, ctypes.c_uint32, I32P, ctypes.c_int32,
                                         ctypes.c_int, ctypes.c_uint8, ctypes.c_uint32, U8PP, U32P]
    lib.kvspaceXvalueEncodeMode.restype = ctypes.c_int
    lib.kvspaceDecodeHead.argtypes = [U8P, ctypes.c_uint32, ctypes.POINTER(HeadV)]
    lib.kvspaceDecodeHead.restype = ctypes.c_int


def encode(lib, kind, raw, dims=(), ref=0, ro=0, vid=0):
    raw_arr = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw)
    ndim = len(dims)
    dims_arr = (ctypes.c_int32 * ndim)(*dims) if ndim > 0 else None
    out = U8P()
    out_len = ctypes.c_uint32()
    rc = lib.kvspaceXvalueEncodeMode(kind.encode(), raw_arr, len(raw), dims_arr, ndim,
                                  ref, ro, vid, ctypes.byref(out), ctypes.byref(out_len))
    assert rc == 0, f"encode({kind}) rc={rc}"
    data = ctypes.string_at(out, out_len.value)
    _libc.free(ctypes.cast(out, ctypes.c_void_p))
    return data


def decode(lib, data):
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    h = HeadV()
    rc = lib.kvspaceDecodeHead(buf, len(data), ctypes.byref(h))
    assert rc == 0, f"decode rc={rc}"
    return {
        "headlen": h.headlen, "ref": h.ref, "storetype": h.storetype,
        "langtype": bytes(h.langtype).split(b"\0", 1)[0].decode(),
        "ndim": h.ndim, "dims": list(h.dims)[:h.ndim],
        "ro": h.ro, "vid": h.vid,
        "body_len": h.body_len, "body_offset": h.body_offset,
    }


def test_kvspace_c_alignment():
    """kvspace-c 与 kvspace-durable 对同一 head 输入产出字节级一致的结果。"""
    if not DURABLE_SO.exists():
        subprocess.run(["cargo", "build"], cwd=ROOT, check=True, capture_output=True)
    if not KVSPACE_C_SO.exists():
        subprocess.run(["make", "kvspace-c"], cwd=KVSPACE_C_DIR / "build", check=True, capture_output=True)

    # kvspace-c 依赖 blockmalloc/slotsboxmalloc，先按绝对路径预加载（RTLD_GLOBAL），
    # 让后续 dlopen kvspace-c 时按 SONAME 命中已加载对象。
    for dep in ["blockmalloc/build/libblockmalloc.so.1", "slotsboxmalloc/build/libslotsboxmalloc.so"]:
        ctypes.CDLL(str(KVSPACE_C_DIR.parent / dep), mode=ctypes.RTLD_GLOBAL)

    lib_d = ctypes.CDLL(str(DURABLE_SO))
    lib_c = ctypes.CDLL(str(KVSPACE_C_SO))
    setup_lib(lib_d)
    setup_lib(lib_c)

    cases = [
        ("int64", struct.pack("<q", 42), (), 0, 0, 0),
        ("int64", struct.pack("<q", 7), (), 0, 1, 0x12345678),
        ("char/utf8", b"hello", (5,), 0, 1, 7),
        ("int32", struct.pack("<6i", 1, 2, 3, 4, 5, 6), (2, 3), 0, 0, 0),
        ("int64", b"/target", (), 1, 0, 9),
        ("int64", b"/gpu/tensor", (), 2, 0, 3),
    ]
    ok = True
    for kind, raw, dims, ref, ro, vid in cases:
        label = f"align {kind} ref={ref} ro={ro} vid={vid}"
        bd = encode(lib_d, kind, raw, dims, ref, ro, vid)
        bc = encode(lib_c, kind, raw, dims, ref, ro, vid)
        if bd != bc:
            print(f"FAIL  {label} (bytes differ)")
            ok = False
            continue
        if decode(lib_d, bd) != decode(lib_c, bc):
            print(f"FAIL  {label} (decode differs)")
            ok = False
            continue
        print(f"PASS  {label}")
    return ok


# ── 写侧落盘时机（issue #23）─────────────────────────────────────────
#
# 写把 XValue 字节攒在句柄内（body 由调用方在返回后填，返回前无法落盘）：下一次 op 落盘上一笔
# （读也是 op），另一个句柄随即可见；漏调 Close 而正常退出时由前端退出兜底落盘。子进程必须
# 独立——它正是「不 Close 就退出」的那一个。

CHILD = r'''
import ctypes, sys
U8P = ctypes.POINTER(ctypes.c_uint8)
lib = ctypes.CDLL(sys.argv[1])
lib.kvspaceConnect.restype = ctypes.c_void_p
lib.kvspaceConnect.argtypes = [ctypes.c_char_p]
lib.kvspaceDelTree.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32]
lib.kvspaceWriteNewPlace.argtypes = [
    ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8, ctypes.c_uint8, ctypes.c_uint8,
    ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32,
    ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32]
lib.kvspaceGet.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int,
    ctypes.POINTER(U8P), ctypes.POINTER(ctypes.c_uint32)]
err = ctypes.create_string_buffer(256)

def put(h, key, val):
    body = U8P()
    lt = ("[%d]char/utf8" % len(val)).encode()
    assert lib.kvspaceWriteNewPlace(h, key, 0, 2, 0, 0, lt, len(val),
                                    ctypes.byref(body), err, 256) == 0, err.value
    ctypes.memmove(body, val, len(val))

def get(h, key):
    out, n = U8P(), ctypes.c_uint32()
    lib.kvspaceGet(h, key, 0, ctypes.byref(out), ctypes.byref(n))
    return None if not out else ctypes.string_at(out, n.value)

h1 = lib.kvspaceConnect(sys.argv[2].encode())
lib.kvspaceDelTree(h1, b"/flushprobe/", err, 256)
put(h1, b"/flushprobe/a", b"va")
put(h1, b"/flushprobe/b", b"vb")
put(h1, b"/flushprobe/c", b"vc")
get(h1, b"/flushprobe/a")          # 任意一次 op 先把上一笔（c）落盘
h2 = lib.kvspaceConnect(sys.argv[2].encode())
print("next-op-visible", get(h2, b"/flushprobe/c") is not None)
put(h1, b"/flushprobe/d", b"vd")   # 留作未决写：不再发 op、不 Close，直接正常退出
sys.exit(0)
'''


def test_exit_flush():
    """写侧落盘时机：下一次 op 即落盘上一笔；漏调 Close 正常退出不丢最后一笔。"""
    if not Path(FRONTEND_SO).exists():
        print(f'FAIL  exit-flush (缺前端 {FRONTEND_SO})')
        return False
    kvbin = os.path.expanduser('~/.local/bin/kvspace')
    env = os.environ.copy()
    keys = ['/flushprobe/a', '/flushprobe/b', '/flushprobe/c', '/flushprobe/d']
    with tempfile.TemporaryDirectory() as tmp:
        cases = [
            ('redis', env.get('KVSPACE', 'redis://127.0.0.1:6379')),
            ('fs', f'fs://{tmp}/fs'),
            ('shm', f'shm://{tmp}/probe.shm'),
        ]
        ok = True
        for label, dsn in cases:
            child = subprocess.run([sys.executable, '-c', CHILD, FRONTEND_SO, dsn],
                                   capture_output=True, text=True, timeout=30)
            seen = 'next-op-visible True' in child.stdout
            # 子进程已退出（未 Close）：经 CLI 复核四笔全在，最后一笔由退出兜底落盘。
            got = subprocess.run([kvbin, 'get', *keys], capture_output=True, text=True,
                                 timeout=10, env={**env, 'KVSPACE': dsn})
            kept = [l for l in got.stdout.splitlines() if 'char/utf8:v' in l]
            if seen and len(kept) == len(keys):
                print(f'PASS  exit-flush {label}')
            else:
                print(f'FAIL  exit-flush {label} (next-op-visible={seen}, 落地 {len(kept)}/{len(keys)})')
                if child.stderr:
                    print(f'  child stderr: {child.stderr.strip()}')
                ok = False
        return ok


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    scripts = sorted(
        os.path.join(here, f) for f in os.listdir(here)
        if f.endswith('.sh')
    )
    if not scripts:
        print('no scripts found')
        sys.exit(1)

    results = [test_script(s) for s in scripts]
    results.append(test_kvspace_c_alignment())
    results.append(test_exit_flush())
    passed = sum(results)
    print(f'\n{passed}/{len(results)} passed')
    sys.exit(0 if passed == len(results) else 1)

if __name__ == '__main__':
    main()
