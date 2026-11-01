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
_bind(_lib.kvspaceShmMkindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
_bind(_lib.kvspaceShmList, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_bool, ctypes.c_int,
                           ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int32)], ctypes.c_int)
_bind(_lib.kvspaceShmExtindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p,
                               ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
_bind(_lib.kvspaceShmDelextindex, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspaceShmNotify, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32], ctypes.c_int)
_bind(_lib.kvspaceShmWatch, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)], ctypes.POINTER(ctypes.c_uint8))

# ── 权威 head 构造器绑定（kvspace ABI，无 handle）──────────────────
# #566/#571: Fixed 8-byte header [len:u48][storetype][ptr], body at offset 8, attr at
# 8+len ([attr_len:u16][records], record = [tag:u8][len:u16][data], tag 1 = langtype).
# Constructed via C kvspaceXvalueHeadNew*; parsed locally on read.

_U8P = ctypes.POINTER(ctypes.c_uint8)
_U8PP = ctypes.POINTER(_U8P)
_U64P = ctypes.POINTER(ctypes.c_uint64)
_bind(_lib.kvspaceXvalueHeadNewScalar,
      [ctypes.c_uint8, _U8P, ctypes.c_uint32, ctypes.c_char_p, _U8PP, _U64P], ctypes.c_int)
_bind(_lib.kvspaceXvalueHeadNewSlack,
      [_U8P, ctypes.c_uint64, ctypes.c_uint64, ctypes.c_char_p, _U8PP, _U64P], ctypes.c_int)
_bind(_lib.kvspaceXvalueHeadNewPtr,
      [_U8P, ctypes.c_uint32, ctypes.c_char_p, _U8PP, _U64P], ctypes.c_int)
_bind(_lib.kvspaceXvalueHeadNewExt,
      [_U8P, ctypes.c_uint32, ctypes.c_char_p, _U8PP, _U64P], ctypes.c_int)

ST_SHORT, ST_SLACK, ST_LARGE, ST_EXT = 0, 1, 2, 3
PTR_INLINE, PTR_PTR, PTR_EXT = 0, 1, 2
HEAD_LEN = 8

# 存储位置：与 KVSPACE_REF_* 对齐（由 ptr 位推导）。
REF_INLINE, REF_PTR, REF_EXT = 0, 1, 2


# ── XValue helpers ────────────────────────────────────────────────
# Construct via C constructors (_take copies and frees malloc buffer); parse header + attr locally.
# Empty langtype = untyped (empty attr).

ATTR_TAG_LANGTYPE = 1


def _take(out, total: ctypes.c_uint64) -> bytes:
    data = ctypes.string_at(out, total.value)
    _libc.free(out)
    return data


def _new(fn, *args) -> bytes:
    out = _U8P()
    total = ctypes.c_uint64()
    if fn(*args, ctypes.byref(out), ctypes.byref(total)):
        raise ValueError(f"{fn.__name__} failed")
    return _take(out, total)


def _u8buf(raw: bytes):
    if not raw:
        return None
    return ctypes.cast((ctypes.c_uint8 * len(raw)).from_buffer_copy(raw), _U8P)


def _xv_scalar(raw: bytes, langtype: str) -> bytes:
    return _new(_lib.kvspaceXvalueHeadNewScalar, ST_SHORT, _u8buf(raw), len(raw),
                langtype.encode())


def _xv_slack(raw: bytes, langtype: str) -> bytes:
    return _new(_lib.kvspaceXvalueHeadNewSlack, _u8buf(raw), len(raw), len(raw),
                langtype.encode())


def _split_wire(data: Optional[bytes]):
    """Validate and split wire into (storetype, ptr, body, attr); returns None if malformed."""
    if not data or len(data) < HEAD_LEN + 2:
        return None
    body_len = int.from_bytes(data[:6], "little")
    storetype, ptr = data[6], data[7]
    if storetype > ST_EXT or ptr > PTR_EXT or HEAD_LEN + body_len + 2 > len(data):
        return None
    a = HEAD_LEN + body_len
    attr_len = int.from_bytes(data[a:a + 2], "little")
    if attr_len < 2 or a + attr_len != len(data):
        return None
    return storetype, ptr, data[HEAD_LEN:a], data[a:a + attr_len]


def _xv_from_wire(data: Optional[bytes]) -> tuple[int, int, bytes]:
    """Read fixed header -> (ref, storetype, body) excluding attr; returns (0, 0, b"") if invalid."""
    parts = _split_wire(data)
    if parts is None:
        return (0, 0, b"")
    storetype, ptr, body, _ = parts
    ref = REF_PTR if ptr == PTR_PTR else REF_EXT if ptr == PTR_EXT else REF_INLINE
    return (ref, storetype, body)


def xv_langtype(data: Optional[bytes]) -> str:
    """Read attr tag=1 (langtype) record; returns "" if untyped or invalid."""
    parts = _split_wire(data)
    if parts is None:
        return ""
    attr = parts[3]
    p = 2
    while p + 3 <= len(attr):
        tag, rlen = attr[p], int.from_bytes(attr[p + 1:p + 3], "little")
        if p + 3 + rlen > len(attr):
            return ""
        if tag == ATTR_TAG_LANGTYPE:
            return attr[p + 3:p + 3 + rlen].decode()
        p += 3 + rlen
    return ""


def xv_int(v: int, langtype: str = "") -> bytes:
    return _xv_scalar(struct.pack("<q", v), langtype)


def xv_float(v: float, langtype: str = "") -> bytes:
    return _xv_scalar(struct.pack("<d", v), langtype)


def xv_str(s: str, langtype: str = "") -> bytes:
    return _xv_slack(s.encode(), langtype)


def xv_index() -> bytes:
    """Directory value (empty-body SHORT header, untyped)."""
    return _xv_scalar(b"", "")


def xv_link(target: str, langtype: str = "") -> bytes:
    """Pointer value (ptr=PTR): body=target key, langtype=target constraint ("" = unconstrained)."""
    t = target.encode()
    return _new(_lib.kvspaceXvalueHeadNewPtr, _u8buf(t), len(t), langtype.encode())


def xv_ext(locator: str, langtype: str = "") -> bytes:
    """External locator (EXT class): body=locator."""
    loc = locator.encode()
    return _new(_lib.kvspaceXvalueHeadNewExt, _u8buf(loc), len(loc), langtype.encode())


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

    def mkindex(self, path: str, capacity: int = 0):
        _lib.kvspaceShmMkindex(self._kv, path.encode(), capacity)

    def list(self, prefix: str, resolve: bool = True) -> list[str]:
        out = ctypes.c_void_p()
        oc = ctypes.c_int32(0)
        _lib.kvspaceShmList(self._kv, prefix.encode(), False, 1 if resolve else 0, ctypes.byref(out), ctypes.byref(oc))
        if not out.value:
            return []
        # Copy and free the C-allocated name array.
        ptrs = ctypes.cast(out, ctypes.POINTER(ctypes.c_void_p))
        names = [ctypes.string_at(ptrs[i]).decode() for i in range(oc.value)]
        for i in range(oc.value):
            _libc.free(ptrs[i])
        _libc.free(out)
        return names

    # ── Link / ExtIndex ──────────────────────────────────────

    def extindex(self, path: str, extpath: str):
        err = ctypes.create_string_buffer(256)
        if _lib.kvspaceShmExtindex(self._kv, path.encode(), extpath.encode(), err, len(err)):
            raise RuntimeError(err.value.decode() or f"extindex({path}, {extpath}) failed")

    def delextindex(self, path: str):
        _lib.kvspaceShmDelextindex(self._kv, path.encode())
