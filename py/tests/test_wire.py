#!/usr/bin/env python3
"""wrapper <-> C constructor round-trip + ShmSet readback.

固定 16 字节 head：[bodylen:u64 LE@0][attrslen:u16 LE@8][padding:6@10-15]，
body@16，attr 紧随 body 且没有长度前缀，total = 16 + bodylen + attrslen。
record = [tag:u8][len:u16 LE][data]，tag 1 = langtype；tag 2（旧 reallen）已删除，
出现即拒。构造走 kvspaceXvalueHeadNew*，读侧本地独立解析。
"""
import ctypes, os, struct, sys, tempfile
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import kvspace
from kvspace import (xv_int, xv_float, xv_str, xv_index, xv_link, xv_ext,
                     xv_langtype, _xv_from_wire, KVSpace,
                     HEAD_LEN, PADDING_LEN, ATTR_TAG_LANGTYPE,
                     BANNED_ATTR_TAG_REALLEN, REF_INLINE, REF_PTR, REF_EXT)
_lib = ctypes.CDLL(kvspace._SO)
_libc = ctypes.CDLL(None)
_libc.free.argtypes = [ctypes.c_void_p]
_U8P = ctypes.POINTER(ctypes.c_uint8)
_U8PP = ctypes.POINTER(_U8P)
_U64P = ctypes.POINTER(ctypes.c_uint64)
_CS = ctypes.c_char_p
_lib.kvspaceXvalueHeadNewScalar.argtypes = [_U8P, ctypes.c_uint32, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewScalar.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewSlack.argtypes = [_U8P, ctypes.c_uint64, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewSlack.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewPtr.argtypes = [_U8P, ctypes.c_uint32, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewPtr.restype = ctypes.c_int
_lib.kvspaceXvalueHeadNewExt.argtypes = [_U8P, ctypes.c_uint32, _CS, _U8PP, _U64P]
_lib.kvspaceXvalueHeadNewExt.restype = ctypes.c_int


def c_new(ref, body, langtype=""):
    """C constructor oracle: (ref, body, langtype) -> full wire.

    ref 只决定走哪个构造器（NewPtr/NewExt 会补 `*`/`@` 前缀），head 里没有 ptr 位。
    """
    buf = (ctypes.c_uint8 * len(body)).from_buffer_copy(body) if body else None
    rp = ctypes.cast(buf, _U8P) if buf is not None else None
    lt = langtype.encode()
    out = _U8P()
    total = ctypes.c_uint64()
    if ref == REF_PTR:
        rc = _lib.kvspaceXvalueHeadNewPtr(rp, len(body), lt, ctypes.byref(out), ctypes.byref(total))
    elif ref == REF_EXT:
        rc = _lib.kvspaceXvalueHeadNewExt(rp, len(body), lt, ctypes.byref(out), ctypes.byref(total))
    else:
        rc = _lib.kvspaceXvalueHeadNewSlack(rp, len(body), lt,
                                            ctypes.byref(out), ctypes.byref(total))
    assert rc == 0, f"c_new(ref={ref}) rc={rc}"
    data = ctypes.string_at(out, total.value)
    _libc.free(out)
    return data


def c_peek(data):
    """独立 wire 解析器 -> (ref, body, langtype)，并校验 head/attr 自洽。

    只读 head 与 attr 区，不调用 wrapper 的解析函数——两边是各自独立的实现。
    """
    assert len(data) >= HEAD_LEN, "shorter than one head"
    body_len = int.from_bytes(data[0:8], "little")
    attr_len = int.from_bytes(data[8:10], "little")
    padding = data[10:10 + PADDING_LEN]
    assert padding == b"\x00" * PADDING_LEN, f"padding must be zero, got {padding!r}"
    a = HEAD_LEN + body_len
    assert a + attr_len == len(data), f"total length mismatch {len(data)} != {a}+{attr_len}"
    attr = data[a:a + attr_len]
    lt = ""
    p = 0
    while p + 3 <= len(attr):
        tag, rlen = attr[p], int.from_bytes(attr[p + 1:p + 3], "little")
        assert tag != BANNED_ATTR_TAG_REALLEN, "tag 2 (old reallen) must not appear"
        if tag == ATTR_TAG_LANGTYPE:
            lt = attr[p + 3:p + 3 + rlen].decode()
        p += 3 + rlen
    if lt[:1] == "*":
        ref = REF_PTR
    elif lt[:1] == "@":
        ref = REF_EXT
    else:
        ref = REF_INLINE
    return (ref, data[HEAD_LEN:a], lt)


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
    check("int64", c_peek(xv_int(42, "int64")), (REF_INLINE, struct.pack("<q", 42), "int64"))
    check("int64.untyped", c_peek(xv_int(42)), (REF_INLINE, struct.pack("<q", 42), ""))
    check("float64", c_peek(xv_float(1.5, "float64")),
          (REF_INLINE, struct.pack("<d", 1.5), "float64"))
    check("str", c_peek(xv_str("héllo", "char/utf8")),
          (REF_INLINE, "héllo".encode(), "char/utf8"))
    check("index", c_peek(xv_index()), (REF_INLINE, b"", ""))
    # 引用性由构造器补的前缀表达：NewPtr -> `*int64`，NewExt -> 裸 `@`。
    check("link", c_peek(xv_link("/t/x", "int64")), (REF_PTR, b"/t/x", "*int64"))
    check("ext", c_peek(xv_ext("s3://b/k")), (REF_EXT, b"s3://b/k", "@"))

    print("-- C constructor -> wrapper read --")
    v = c_new(REF_INLINE, struct.pack("<q", 7), "int64")
    check("int64", _xv_from_wire(v), (REF_INLINE, struct.pack("<q", 7)))
    check("int64.langtype", xv_langtype(v), "int64")
    v = c_new(REF_INLINE, "héllo".encode(), "char/utf8")
    check("str", _xv_from_wire(v), (REF_INLINE, "héllo".encode()))
    check("str.langtype", xv_langtype(v), "char/utf8")
    check("ptr", _xv_from_wire(c_new(REF_PTR, b"/t/x")), (REF_PTR, b"/t/x"))
    check("ext", _xv_from_wire(c_new(REF_EXT, b"s3://b/k")), (REF_EXT, b"s3://b/k"))
    check("none", _xv_from_wire(c_new(REF_INLINE, b"")), (REF_INLINE, b""))
    check("garbage", _xv_from_wire(b"\x00\x01\x02\x03"), (REF_INLINE, b""))
    # 16 字节 head 的 bodylen=1、attrslen=0、padding 全零，body=0x2a：
    check("minimal", _xv_from_wire(
        (1).to_bytes(8, "little") + (0).to_bytes(2, "little") + b"\x00" * 6 + b"\x2a"),
        (REF_INLINE, b"\x2a"))

    print("-- ShmSet + readback --")
    d = tempfile.mkdtemp()
    kv = KVSpace(os.path.join(d, "db"))  # data_size must be 8 * 64^n, default 32768
    kv.set("/x", xv_int(99, "int64"))
    got = kv.get("/x", resolve=False)
    check("shm.nonnull", got is not None, True)
    check("shm.value", _xv_from_wire(got), (REF_INLINE, struct.pack("<q", 99)))
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
