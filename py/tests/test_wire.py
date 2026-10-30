#!/usr/bin/env python3
"""wrapper ↔ C 构造器双向对拍 + ShmSet 读回。

#566：head 固定 8 字节 [len:u48][storetype][ptr]，body 偏移恒 8。直接 memcpy
struct + body——C 侧权威是 kvspaceXvalueHeadNew* 构造器；wrapper 构造走同一构造器，
读取本地 memcpy 固定头 + 字段访问。断言 ref/storetype/body。
"""
import ctypes, os, struct, sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kvspace
from kvspace import (xv_int, xv_float, xv_str, xv_index, xv_link, xv_ext,
                     _xv_from_wire, KVSpace, ST_SHORT, ST_SLACK, ST_EXT,
                     PTR_INLINE, PTR_PTR, PTR_EXT, HEAD_LEN)

_lib = ctypes.CDLL(kvspace._SO)
_libc = ctypes.CDLL(None)
_libc.free.argtypes = [ctypes.c_void_p]

_U8P = ctypes.POINTER(ctypes.c_uint8)
_U8PP = ctypes.POINTER(_U8P)
_U64P = ctypes.POINTER(ctypes.c_uint64)
_lib.kvspaceXvalueHeadNewScalar.argtypes = [ctypes.c_uint8, _U8P, ctypes.c_uint32, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewScalar.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewSlack.argtypes = [_U8P, ctypes.c_uint64, ctypes.c_uint64, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewSlack.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewPtr.argtypes = [_U8P, ctypes.c_uint32, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewPtr.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewExt.argtypes = [_U8P, ctypes.c_uint32, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewExt.restype = ctypes.c_int


def c_new(storetype, ptr, body):
    """C 侧构造（独立 oracle）：(storetype, ptr, body) → 固定头 + body。"""
    buf = (ctypes.c_uint8 * len(body)).from_buffer_copy(body) if body else None
    rp = ctypes.cast(buf, _U8P) if buf is not None else None
    out = _U8P()
    total = ctypes.c_uint64()
    if ptr == PTR_PTR:
        rc = _lib.kvspaceXvalueHeadNewPtr(rp, len(body), ctypes.byref(out), ctypes.byref(total))
    elif ptr == PTR_EXT:
        rc = _lib.kvspaceXvalueHeadNewExt(rp, len(body), ctypes.byref(out), ctypes.byref(total))
    else:
        rc = _lib.kvspaceXvalueHeadNewScalar(storetype, rp, len(body),
                                             ctypes.byref(out), ctypes.byref(total))
    assert rc == 0, f"c_new(st={storetype}, ptr={ptr}) rc={rc}"
    data = ctypes.string_at(out, total.value)
    _libc.free(out)
    return data


def c_peek(data):
    """本地解析固定头 → (ref, storetype, body)。"""
    assert len(data) >= HEAD_LEN, "短于 8 字节头"
    body_len = int.from_bytes(data[:6], "little")
    storetype, ptr = data[6], data[7]
    assert HEAD_LEN + body_len == len(data), f"总长不匹配 {len(data)}"
    ref = 1 if ptr == PTR_PTR else 2 if ptr == PTR_EXT else 0
    return (ref, storetype, data[HEAD_LEN:])


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
    print("── wrapper 构造 → C 头解析 ──")
    r, st, body = c_peek(xv_int(42))
    check("int64.ref", r, 0)
    check("int64.storetype", st, ST_SHORT)
    check("int64.body", body, struct.pack("<q", 42))

    r, st, body = c_peek(xv_float(1.5))
    check("float64.ref", r, 0)
    check("float64.storetype", st, ST_SHORT)
    check("float64.body", body, struct.pack("<d", 1.5))

    r, st, body = c_peek(xv_str("héllo"))
    check("str.ref", r, 0)
    check("str.storetype", st, ST_SLACK)
    check("str.body", body, "héllo".encode())

    r, st, body = c_peek(xv_index())
    check("index.ref", r, 0)
    check("index.storetype", st, ST_SHORT)
    check("index.body", body, b"")

    r, st, body = c_peek(xv_link("/t/x"))
    check("link.ref(ptr)", r, 1)
    check("link.storetype", st, ST_SLACK)
    check("link.body", body, b"/t/x")

    r, st, body = c_peek(xv_ext("s3://b/k"))
    check("ext.ref", r, 2)
    check("ext.storetype", st, ST_EXT)
    check("ext.body", body, b"s3://b/k")

    print("── C 构造器 → wrapper 读取 ──")
    r, st, body = _xv_from_wire(c_new(ST_SHORT, PTR_INLINE, struct.pack("<q", 7)))
    check("int64.ref", r, 0)
    check("int64.storetype", st, ST_SHORT)
    check("int64.body", body, struct.pack("<q", 7))

    r, st, body = _xv_from_wire(c_new(ST_SLACK, PTR_INLINE, "héllo".encode()))
    check("str.storetype", st, ST_SLACK)
    check("str.body", body, "héllo".encode())

    r, st, body = _xv_from_wire(c_new(ST_SLACK, PTR_PTR, b"/t/x"))
    check("ptr.ref", r, 1)
    check("ptr.body", body, b"/t/x")

    r, st, body = _xv_from_wire(c_new(ST_EXT, PTR_EXT, b"s3://b/k"))
    check("ext.ref", r, 2)
    check("ext.storetype", st, ST_EXT)
    check("ext.body", body, b"s3://b/k")

    check("garbage.kind", _xv_from_wire(b"\x00\x01\x02\x03"), (0, 0, b""))
    check("none.kind", _xv_from_wire(c_new(ST_SHORT, PTR_INLINE, b"")), (0, 0, b""))

    print("── ShmSet + 读回 ──")
    d = tempfile.mkdtemp()
    kv = KVSpace(os.path.join(d, "db"))  # data_size 须为 8·64ⁿ，默认 32768
    kv.set("/x", xv_int(99))
    got = kv.get("/x", resolve=False)
    check("shm.nonnull", got is not None, True)
    r, st, raw = _xv_from_wire(got)
    check("shm.storetype", st, ST_SHORT)
    check("shm.body", raw, struct.pack("<q", 99))
    kv.close()

    print(f"\n{_total - _fails}/{_total} 通过")
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
