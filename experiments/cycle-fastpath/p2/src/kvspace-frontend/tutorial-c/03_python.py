#!/usr/bin/env python3
"""03_python — ctypes 直调 libkvspace（dispatch 前端），shm:// 后端"""

import ctypes
import os
import struct

SO = os.environ.get("KVSPACE_SO", "libkvspace.so")
lib = ctypes.CDLL(SO)

U8P = ctypes.POINTER(ctypes.c_uint8)
U32P = ctypes.POINTER(ctypes.c_uint32)
CCHARPP = ctypes.POINTER(ctypes.c_char_p)


class Head(ctypes.Structure):
    _fields_ = [
        ("kindexpr", ctypes.c_uint8 * 256),
        ("ro", ctypes.c_uint8),
        ("vid", ctypes.c_uint32),
        ("body_len", ctypes.c_int32),
        ("body_offset", ctypes.c_int32),
    ]


def bind(fn, args, restype):
    fn.argtypes = args
    fn.restype = restype


bind(lib.kvspaceConnect, [ctypes.c_char_p], ctypes.c_void_p)
bind(lib.kvspaceClose, [ctypes.c_void_p], None)
# 借用读：out 指向 kvspace 常驻空间，string_at 拷出后不 free。resolve 穿透 link。
bind(lib.kvspaceGet, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(U8P), U32P], ctypes.c_int)
# 写即构造：就地/新位置二选一原语，往返回的 body 偏移指针写字节。
bind(lib.kvspaceWriteInPlace, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint32, ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceWriteNewPlace, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名。
bind(lib.kvspaceListLen, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int32)], ctypes.c_int)
bind(lib.kvspaceListAt, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int32, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.kvspaceDel, [ctypes.c_void_p, CCHARPP, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceDelTree, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceMkindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceNewInt64, [ctypes.c_int64, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.kvspaceNewChar, [U8P, ctypes.c_uint32, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.kvspaceDecodeHead, [U8P, ctypes.c_uint32, ctypes.POINTER(Head)], ctypes.c_int)


def enc_int(v):
    out, n = U8P(), ctypes.c_uint32()
    lib.kvspaceNewInt64(v, ctypes.byref(out), ctypes.byref(n))
    return ctypes.string_at(out, n.value)


def enc_str(s):
    b = s.encode()
    buf = (ctypes.c_uint8 * len(b)).from_buffer_copy(b)
    out, n = U8P(), ctypes.c_uint32()
    lib.kvspaceNewChar(buf, len(b), ctypes.byref(out), ctypes.byref(n))
    return ctypes.string_at(out, n.value)


def decode(data):
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    h = Head()
    lib.kvspaceDecodeHead(buf, len(data), ctypes.byref(h))
    kx = bytes(h.kindexpr).split(b"\0", 1)[0].decode()
    if kx.startswith(("*", "@")):
        kx = kx[1:]
    if kx.startswith("["):
        kx = kx[kx.index("]") + 1:]
    return kx, data[h.body_offset:h.body_offset + h.body_len]


class KV:
    def __init__(self, path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self.path = path
        self.kv = lib.kvspaceConnect(f"shm://{path}".encode())
        if not self.kv:
            raise RuntimeError(f"connect shm://{path} failed")

    def close(self):
        lib.kvspaceClose(self.kv)
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def set(self, key, val):
        buf = (ctypes.c_uint8 * len(val)).from_buffer_copy(val)
        h = Head()
        lib.kvspaceDecodeHead(buf, len(val), ctypes.byref(h))
        bl = h.body_len if h.body_len > 0 else 0
        kindexpr = bytes(h.kindexpr).split(b"\0", 1)[0]
        dst = U8P()
        err = ctypes.create_string_buffer(256)
        if lib.kvspaceWriteInPlace(self.kv, key.encode(), 1, bl, ctypes.byref(dst), err, 256) != 0:
            lib.kvspaceWriteNewPlace(self.kv, key.encode(), kindexpr, bl, ctypes.byref(dst), err, 256)
        if bl > 0 and dst:
            ctypes.memmove(dst, ctypes.byref(buf, h.body_offset), bl)

    def get(self, key):
        out, n = U8P(), ctypes.c_uint32()
        lib.kvspaceGet(self.kv, key.encode(), 0, ctypes.byref(out), ctypes.byref(n))
        if not out or n.value == 0:
            return None
        return ctypes.string_at(out, n.value)

    def delete(self, key):
        keys = (ctypes.c_char_p * 1)(key.encode())
        lib.kvspaceDel(self.kv, keys, 1, None, 0)

    def deltree(self, prefix):
        lib.kvspaceDelTree(self.kv, prefix.encode(), None, 0)

    def mkindex(self, path):
        lib.kvspaceMkindex(self.kv, path.encode(), None, 0)

    def list(self, prefix):
        count = ctypes.c_int32()
        if lib.kvspaceListLen(self.kv, prefix.encode(), 0, 1, ctypes.byref(count)) != 0 or count.value <= 0:
            return []
        names = []
        for i in range(count.value):
            out, n = U8P(), ctypes.c_uint32()
            if lib.kvspaceListAt(self.kv, prefix.encode(), 0, 1, i, ctypes.byref(out), ctypes.byref(n)) == 0 and out:
                names.append(ctypes.string_at(out, n.value).decode())
        return names


with KV("/tmp/kvspace_py.shm") as kv:
    kv.mkindex("/py/")

    kv.set("/py/a", enc_int(42))
    kv.set("/py/b", enc_str("你好"))

    for k in ["/py/a", "/py/b"]:
        kind, raw = decode(kv.get(k))
        if kind == "int64":
            val = struct.unpack("<q", raw)[0]
        else:
            val = raw.decode()
        print(f"{k}\t{kind}:{val}")

    ns = kv.list("/py/")
    print(f"list /py/: {sorted(ns)} (count={len(ns)})")

    kv.delete("/py/b")
    assert len(kv.list("/py/")) == 1
    kv.deltree("/py/")
    assert kv.list("/py/") == []

print("PASS python")
