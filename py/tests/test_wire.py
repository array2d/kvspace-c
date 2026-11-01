#!/usr/bin/env python3
"""wrapper <-> C constructor round-trip + ShmSet readback.
#566/#571: Fixed 8-byte header [len:u48][storetype][ptr], body at offset 8, attr at 8+len
([attr_len:u16][records], tag 1 = langtype), total = 8 + len + attr_len.
Constructed via kvspaceXvalueHeadNew*; parsed locally on read.
"""
import ctypes, os, struct, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kvspace
from kvspace import (xv_int, xv_float, xv_str, xv_index, xv_link, xv_ext,
                     xv_langtype, _xv_from_wire, KVSpace, ST_SHORT, ST_SLACK, ST_EXT,
                     PTR_INLINE, PTR_PTR, PTR_EXT, HEAD_LEN)
_lib = ctypes.CDLL(kvspace._SO)
_libc = ctypes.CDLL(None)
_libc.free.argtypes = [ctypes.c_void_p]
_U8P = ctypes.POINTER(ctypes.c_uint8)
_U8PP = ctypes.POINTER(_U8P)
_U64P = ctypes.POINTER(ctypes.c_uint64)
_CS = ctypes.c_char_p
_lib.kvspaceXvalueHeadNewScalar.argtypes = [ctypes.c_uint8, _U8P, ctypes.c_uint32, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewScalar.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewSlack.argtypes = [_U8P, ctypes.c_uint64, ctypes.c_uint64, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewSlack.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewPtr.argtypes = [_U8P, ctypes.c_uint32, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewPtr.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewExt.argtypes = [_U8P, ctypes.c_uint32, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewExt.restype = ctypes.c_int


def c_new(storetype, ptr, body, langtype=""):
    """C constructor oracle: (storetype, ptr, body, langtype) -> full wire."""
    buf = (ctypes.c_uint8 * len(body)).from_buffer_copy(body) if body else None
    rp = ctypes.cast(buf, _U8P) if buf is not None else None
    lt = langtype.encode()
    out = _U8P()
    total = ctypes.c_uint64()
    if ptr == PTR_PTR:
        rc = _lib.kvspaceXvalueHeadNewPtr(rp, len(body), lt, ctypes.byref(out), ctypes.byref(total))
    elif ptr == PTR_EXT:
        rc = _lib.kvspaceXvalueHeadNewExt(rp, len(body), lt, ctypes.byref(out), ctypes.byref(total))
    elif storetype == ST_SLACK:
        rc = _lib.kvspaceXvalueHeadNewSlack(rp, len(body), len(body), lt,
                                            ctypes.byref(out), ctypes.byref(total))
    else:
        rc = _lib.kvspaceXvalueHeadNewScalar(storetype, rp, len(body), lt,
                                             ctypes.byref(out), ctypes.byref(total))
    assert rc == 0, f"c_new(st={storetype}, ptr={ptr}) rc={rc}"
    data = ctypes.string_at(out, total.value)
    _libc.free(out)
    return data


def c_peek(data):
    """Independent wire parser -> (ref, storetype, body, langtype) with total length check."""
    assert len(data) >= HEAD_LEN + 2, "shorter than 8-byte header + empty attr"
    body_len = int.from_bytes(data[:6], "little")
    storetype, ptr = data[6], data[7]
    a = HEAD_LEN + body_len
    attr_len = int.from_bytes(data[a:a + 2], "little")
    assert a + attr_len == len(data), f"total length mismatch {len(data)} != {a}+{attr_len}"
    lt = ""
    if attr_len > 2:
        assert data[a + 2] == 1, "first attr record must be langtype"
        rlen = int.from_bytes(data[a + 3:a + 5], "little")
        lt = data[a + 5:a + 5 + rlen].decode()
    ref = 1 if ptr == PTR_PTR else 2 if ptr == PTR_EXT else 0
    return (ref, storetype, data[HEAD_LEN:a], lt)


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
    print("-- wrapper construct -> independent parse --")
    check("int64", c_peek(xv_int(42, "int64")), (0, ST_SHORT, struct.pack("<q", 42), "int64"))
    check("int64.untyped", c_peek(xv_int(42)), (0, ST_SHORT, struct.pack("<q", 42), ""))
    check("float64", c_peek(xv_float(1.5, "float64")),
          (0, ST_SHORT, struct.pack("<d", 1.5), "float64"))
    check("str", c_peek(xv_str("héllo", "char/utf8")),
          (0, ST_SLACK, "héllo".encode(), "char/utf8"))
    check("index", c_peek(xv_index()), (0, ST_SHORT, b"", ""))
    check("link", c_peek(xv_link("/t/x", "int64")), (1, ST_SLACK, b"/t/x", "int64"))
    check("ext", c_peek(xv_ext("s3://b/k")), (2, ST_EXT, b"s3://b/k", ""))

    print("-- C constructor -> wrapper read --")
    v = c_new(ST_SHORT, PTR_INLINE, struct.pack("<q", 7), "int64")
    check("int64", _xv_from_wire(v), (0, ST_SHORT, struct.pack("<q", 7)))
    check("int64.langtype", xv_langtype(v), "int64")
    v = c_new(ST_SLACK, PTR_INLINE, "héllo".encode(), "char/utf8")
    check("str", _xv_from_wire(v), (0, ST_SLACK, "héllo".encode()))
    check("str.langtype", xv_langtype(v), "char/utf8")
    check("ptr", _xv_from_wire(c_new(ST_SLACK, PTR_PTR, b"/t/x")), (1, ST_SLACK, b"/t/x"))
    check("ext", _xv_from_wire(c_new(ST_EXT, PTR_EXT, b"s3://b/k")), (2, ST_EXT, b"s3://b/k"))
    check("none", _xv_from_wire(c_new(ST_SHORT, PTR_INLINE, b"")), (0, ST_SHORT, b""))
    check("garbage", _xv_from_wire(b"\x00\x01\x02\x03"), (0, 0, b""))
    check("no-attr", _xv_from_wire(b"\x01\x00\x00\x00\x00\x00\x00\x00\x2a"), (0, 0, b""))

    print("-- ShmSet + readback --")
    d = tempfile.mkdtemp()
    kv = KVSpace(os.path.join(d, "db"))  # data_size must be 8 * 64^n, default 32768
    kv.set("/x", xv_int(99, "int64"))
    got = kv.get("/x", resolve=False)
    check("shm.nonnull", got is not None, True)
    check("shm.value", _xv_from_wire(got), (0, ST_SHORT, struct.pack("<q", 99)))
    check("shm.langtype", xv_langtype(got), "int64")
    kv.mkindex("/d/")
    kv.set("/d/a", xv_int(1))
    kv.set("/d/b", xv_str("s"))
    check("shm.list", kv.list("/d/"), ["a", "b"])
    try:
        kv.extindex("/e/", "/missing/")
        check("shm.extindex.missing", "no error", "RuntimeError")
    except RuntimeError as e:
        check("shm.extindex.missing", "missing" in str(e), True)
    kv.close()
    print(f"\n{_total - _fails}/{_total} passed")
    return 1 if _fails else 0


if __name__ == "__main__":
    sys.exit(main())
