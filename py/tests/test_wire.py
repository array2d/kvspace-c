#!/usr/bin/env python3
"""wrapper ↔ C 权威 codec 双向对拍 + ShmSet 读回。

方向一：wrapper 编码 → C `kvspaceDecodeHead` 解头，断言 ref/class/langtype/body。
方向二：C `kvspaceTlvEncodeMode` 编码 → wrapper `_xv_decode` 解头，断言 kind/array_len/body。

独立 oracle：测试自行绑定 C 符号，不经 wrapper 的编解码辅助。
"""
import ctypes, os, struct, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kvspace
from kvspace import (xv_int, xv_float, xv_str, xv_index, xv_link, xv_ext, xv_map,
                     _xv_decode, KVSpace)

_lib = ctypes.CDLL(kvspace._SO)
_libc = ctypes.CDLL(None)
_libc.free.argtypes = [ctypes.c_void_p]


class Head(ctypes.Structure):
    _fields_ = [
        ("headlen", ctypes.c_uint16), ("ref", ctypes.c_uint8),
        ("storetype", ctypes.c_uint8), ("ro", ctypes.c_uint8),
        ("vid", ctypes.c_uint32), ("body_len", ctypes.c_int32),
        ("ndim", ctypes.c_int32), ("dims", ctypes.c_int32 * 8),
        ("langtype", ctypes.c_char * 256), ("langtype_len", ctypes.c_int32),
        ("body_offset", ctypes.c_int32), ("body_cap", ctypes.c_uint64),
    ]

_U8P = ctypes.POINTER(ctypes.c_uint8)
_lib.kvspaceTlvEncodeMode.argtypes = [ctypes.c_char_p, _U8P, ctypes.c_uint32,
                                      ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
                                      ctypes.c_int32, ctypes.c_uint8, ctypes.c_uint32,
                                      ctypes.POINTER(_U8P), ctypes.POINTER(ctypes.c_uint32)]
_lib.kvspaceTlvEncodeMode.restype = ctypes.c_int
_lib.kvspaceDecodeHead.argtypes = [_U8P, ctypes.c_uint32, ctypes.POINTER(Head)]
_lib.kvspaceDecodeHead.restype = ctypes.c_int


def c_encode(kind, raw=b"", dims=(), ref=0):
    buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
    d = (ctypes.c_int32 * len(dims))(*dims) if dims else None
    out = _U8P()
    ol = ctypes.c_uint32()
    rc = _lib.kvspaceTlvEncodeMode(kind.encode(),
                                   ctypes.cast(buf, _U8P) if buf is not None else None,
                                   len(raw), d, len(dims), ref, 0, 0,
                                   ctypes.byref(out), ctypes.byref(ol))
    assert rc == 0, f"c_encode({kind}) rc={rc}"
    data = ctypes.string_at(out, ol.value)
    _libc.free(out)
    return data


def c_decode(data):
    """→ (ref, storetype, ndim, dims, langtype, body)"""
    b = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    h = Head()
    assert _lib.kvspaceDecodeHead(b, len(data), ctypes.byref(h)) == 0, "c_decode 失败"
    lt = h.langtype[:h.langtype_len].decode()
    return (h.ref, h.storetype, h.ndim, list(h.dims[:h.ndim]), lt,
            bytes(data[h.body_offset:h.body_offset + h.body_len]))


_fails = 0
_total = 0


def check(name, got, want):
    global _fails, _total
    _total += 1
    if got == want:
        print(f"  PASS {name}")
    else:
        _fails += 1
        print(f"  FAIL {name}: got {got!r}, want {want!r}")


def main():
    print("── wrapper 编码 → C 解码 ──")
    r, st, nd, dims, lt, body = c_decode(xv_int(42))
    check("int64.ref", r, 0)
    check("int64.class", st, 0)
    check("int64.langtype", lt, "int64")
    check("int64.body", body, struct.pack("<q", 42))

    r, st, _, _, lt, _ = c_decode(xv_float(1.5))
    check("float64.class", st, 0)
    check("float64.langtype", lt, "float64")

    r, st, _, _, lt, body = c_decode(xv_str("héllo"))
    check("str.class", st, 1)  # slack
    check("str.langtype", lt, "[5]char/utf8")
    check("str.body", body, "héllo".encode())

    r, st, _, _, lt, body = c_decode(xv_index())
    check("index.class", st, 0)
    check("index.langtype", lt, "lib")
    check("index.body", body, b"")

    r, st, _, _, lt, body = c_decode(xv_link("/t/x"))
    check("link.ref(ptr)", r, 1)
    check("link.class", st, 1)
    check("link.langtype", lt, "lib")
    check("link.body", body, b"/t/x")

    r, st, _, _, lt, body = c_decode(xv_ext("s3://b/k"))
    check("ext.ref", r, 2)
    check("ext.class", st, 3)
    check("ext.body", body, b"s3://b/k")

    r, st, _, _, lt, _ = c_decode(xv_map("[int64]·int64"))
    check("map.class", st, 0)
    check("map.langtype", lt, "[int64]·int64")

    print("── C 编码 → wrapper 解码 ──")
    check("int64.kind", _xv_decode(c_encode("int64", struct.pack("<q", 7)))[0], "int64")
    check("int64.al", _xv_decode(c_encode("int64", struct.pack("<q", 7)))[1], 1)

    k, al, raw = _xv_decode(c_encode("char/utf8", "héllo".encode()))
    check("str.kind", k, "char/utf8")
    check("str.al(chars)", al, 5)
    check("str.body", raw, "héllo".encode())

    raw48 = bytes(range(48))
    t = c_encode("int64", raw48, dims=(2, 3))
    _, st, nd, dims, lt, _ = c_decode(t)
    check("tensor.class", st, 2)
    check("tensor.ndim", nd, 2)
    check("tensor.dims", dims, [2, 3])
    check("tensor.langtype", lt, "[2,3]int64")
    k, al, raw = _xv_decode(t)
    check("tensor.kind", k, "int64")
    check("tensor.al", al, 6)
    check("tensor.body", raw, raw48)

    k, _, raw = _xv_decode(c_encode("lib", b"/t/x", ref=1))
    check("ptr.kind", k, "lib")
    check("ptr.body", raw, b"/t/x")

    check("bool.kind", _xv_decode(c_encode("bool", b"\x01"))[0], "bool")
    check("none.kind", _xv_decode(c_encode("None"))[0], "")
    check("garbage.kind", _xv_decode(b"\x00\x01\x02\x03")[0], "")

    print("── ShmSet + 读回 ──")
    d = tempfile.mkdtemp()
    kv = KVSpace(os.path.join(d, "db"))  # data_size 须为 8·64ⁿ，默认 32768
    kv.set("/x", xv_int(99))
    got = kv.get("/x", resolve=False)
    check("shm.nonnull", got is not None, True)
    k, al, raw = _xv_decode(got)
    check("shm.kind", k, "int64")
    check("shm.body", raw, struct.pack("<q", 99))
    kv.close()

    print(f"\n{_total - _fails}/{_total} 通过")
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
