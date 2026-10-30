"""
kvspace-c Python wrapper — ctypes FFI over libkvspace-c (.so / .dylib)
"""

import ctypes, os, struct, sys
from pathlib import Path
from typing import Optional

_SO = os.environ.get("KVSPACE_C_SO")
if not _SO:
    _ext = ".dylib" if sys.platform == "darwin" else ".so"
    _SO = str(Path(__file__).resolve().parent.parent.parent / "build" / f"libkvspace-c{_ext}")
_lib = ctypes.CDLL(_SO)
# codec 产出为 frontend malloc 缓冲，拷出后以 libc free 释放。
_libc = ctypes.CDLL(None)
_libc.free.argtypes = [ctypes.c_void_p]
_libc.free.restype = None


def _bind(fn, argtypes, restype):
    fn.argtypes = argtypes
    fn.restype = restype


_bind(_lib.kvspaceShmOpen, [ctypes.c_char_p, ctypes.c_size_t], ctypes.c_void_p)
_bind(_lib.kvspaceShmClose, [ctypes.c_void_p], None)
_bind(_lib.kvspaceShmGet, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int32)], ctypes.POINTER(ctypes.c_uint8))
_bind(_lib.kvspaceShmSet, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32], ctypes.c_int)
_bind(_lib.kvspaceShmDel, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspaceShmDeltree, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspaceShmMkindex, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspaceShmList, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_bool, ctypes.c_int,
                           ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int32)], ctypes.c_int)
_bind(_lib.kvspaceShmExtindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspaceShmDelextindex, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspaceShmNotify, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32], ctypes.c_int)
_bind(_lib.kvspaceShmWatch, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)], ctypes.POINTER(ctypes.c_uint8))

# ── 权威 head 构造器绑定（kvspace ABI，无 handle）──────────────────
# #566：head 固定 8 字节 [len:u48][storetype][ptr]，body 偏移恒 8。直接 memcpy
# struct + body——构造走 C 构造器 kvspaceXvalueHeadNew*，读取本地 memcpy 固定头 + 字段访问。

_U8P = ctypes.POINTER(ctypes.c_uint8)
_U8PP = ctypes.POINTER(_U8P)
_U64P = ctypes.POINTER(ctypes.c_uint64)
_bind(_lib.kvspaceXvalueHeadNewScalar,
      [ctypes.c_uint8, _U8P, ctypes.c_uint32, _U8PP, _U64P], ctypes.c_int)
_bind(_lib.kvspaceXvalueHeadNewSlack,
      [_U8P, ctypes.c_uint64, ctypes.c_uint64, _U8PP, _U64P], ctypes.c_int)
_bind(_lib.kvspaceXvalueHeadNewPtr,
      [_U8P, ctypes.c_uint32, _U8PP, _U64P], ctypes.c_int)
_bind(_lib.kvspaceXvalueHeadNewExt,
      [_U8P, ctypes.c_uint32, _U8PP, _U64P], ctypes.c_int)

ST_SHORT, ST_SLACK, ST_LARGE, ST_EXT = 0, 1, 2, 3
PTR_INLINE, PTR_PTR, PTR_EXT = 0, 1, 2
HEAD_LEN = 8

# 存储位置：与 KVSPACE_REF_* 对齐（由 ptr 位推导）。
REF_INLINE, REF_PTR, REF_EXT = 0, 1, 2


# ── XValue helpers ────────────────────────────────────────────────
# 构造走 C 构造器（输出 frontend malloc，_take 拷出后 free）；读取本地
# memcpy 固定头 + 字段访问。

def _take(out, total: ctypes.c_uint64) -> bytes:
    data = ctypes.string_at(out, total.value)
    _libc.free(out)
    return data


def _new(fn, *args) -> bytes:
    out = _U8P()
    total = ctypes.c_uint64()
    if fn(*args, ctypes.byref(out), ctypes.byref(total)):
        raise ValueError(f"{fn.__name__} 构造失败")
    return _take(out, total)


def _xv_scalar(raw: bytes) -> bytes:
    buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
    return _new(_lib.kvspaceXvalueHeadNewScalar, ST_SHORT,
                ctypes.cast(buf, _U8P) if buf is not None else None, len(raw))


def _xv_slack(raw: bytes) -> bytes:
    buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
    return _new(_lib.kvspaceXvalueHeadNewSlack,
                ctypes.cast(buf, _U8P) if buf is not None else None, len(raw), len(raw))


def _xv_from_wire(data: Optional[bytes]) -> tuple[int, int, bytes]:
    """读固定头 → (ref, storetype, body)。langtype 不在 head，kind 无从解析。"""
    if not data or len(data) < HEAD_LEN:
        return (0, 0, b"")
    body_len = int.from_bytes(data[:6], "little")
    storetype, ptr = data[6], data[7]
    if storetype > ST_EXT or ptr > PTR_EXT or HEAD_LEN + body_len != len(data):
        return (0, 0, b"")
    ref = REF_PTR if ptr == PTR_PTR else REF_EXT if ptr == PTR_EXT else REF_INLINE
    return (ref, storetype, data[HEAD_LEN:])


def xv_int(v: int) -> bytes:
    return _xv_scalar(struct.pack("<q", v))


def xv_float(v: float) -> bytes:
    return _xv_scalar(struct.pack("<d", v))


def xv_str(s: str) -> bytes:
    return _xv_slack(s.encode())


def xv_index() -> bytes:
    """目录值（空 body 的 SHORT 头）。"""
    return _xv_scalar(b"")


def xv_link(target: str) -> bytes:
    """指针值（ptr 位）：body=目标 key。langtype 由调用方写 x‥langtype attr key。"""
    buf = (ctypes.c_uint8 * len(target)).from_buffer_copy(target.encode())
    return _new(_lib.kvspaceXvalueHeadNewPtr, buf, len(target))


def xv_ext(locator: str) -> bytes:
    """外部定位符（EXT class）：body=locator。langtype 由调用方写 x‥langtype attr key。"""
    buf = (ctypes.c_uint8 * len(locator)).from_buffer_copy(locator.encode())
    return _new(_lib.kvspaceXvalueHeadNewExt, buf, len(locator))


# ── KVSpace ─────────────────────────────────────────────────────

class KVSpace:
    def __init__(self, path: str, data_size: int = 32768):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self._path = path
        self._kv = _lib.kvspaceShmOpen(path.encode(), data_size)
        if not self._kv:
            raise RuntimeError(f"kvspaceShmOpen({path}) failed")

    def close(self):
        _lib.kvspaceShmClose(self._kv)
        try:
            os.unlink(self._path)
        except FileNotFoundError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    # ── CRUD ─────────────────────────────────────────────────

    def get(self, key: str, resolve: bool = True) -> Optional[bytes]:
        ol = ctypes.c_int32(0)
        p = _lib.kvspaceShmGet(self._kv, key.encode(), 1 if resolve else 0, ctypes.byref(ol))
        return ctypes.string_at(p, ol.value) if p and ol.value else None

    def set(self, key: str, val: bytes):
        _lib.kvspaceShmSet(self._kv, key.encode(),
                         ctypes.cast(ctypes.c_char_p(val), ctypes.POINTER(ctypes.c_uint8)),
                         len(val))

    def delete(self, key: str):
        _lib.kvspaceShmDel(self._kv, key.encode())

    def deltree(self, prefix: str):
        _lib.kvspaceShmDeltree(self._kv, prefix.encode())

    # ── Directory ────────────────────────────────────────────

    def mkindex(self, path: str):
        _lib.kvspaceShmMkindex(self._kv, path.encode())

    def list(self, prefix: str, resolve: bool = True) -> list[str]:
        out = ctypes.c_void_p()
        oc = ctypes.c_int32(0)
        _lib.kvspaceShmList(self._kv, prefix.encode(), False, 1 if resolve else 0, ctypes.byref(out), ctypes.byref(oc))
        if oc.value == 0:
            return []
        ptrs = ctypes.cast(out, ctypes.POINTER(ctypes.c_char_p))
        return [ptrs[i].decode() for i in range(oc.value)]

    # ── Link / ExtIndex ──────────────────────────────────────

    def extindex(self, path: str, extpath: str):
        _lib.kvspaceShmExtindex(self._kv, path.encode(), extpath.encode())

    def delextindex(self, path: str):
        _lib.kvspaceShmDelextindex(self._kv, path.encode())
