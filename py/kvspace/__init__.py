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

# ── 权威 codec 绑定（kvspace ABI，无 handle）──────────────────────
# wire：[pow:u8][flags:u8][a:u64le][b:u64le][langtype][padding][body]，headlen = 1 << pow。

class _Head(ctypes.Structure):
    """对齐 kvspace.h 的 kvspaceHead_t。"""
    _fields_ = [
        ("headlen", ctypes.c_uint16), ("ref", ctypes.c_uint8),
        ("storetype", ctypes.c_uint8), ("ro", ctypes.c_uint8),
        ("vid", ctypes.c_uint32), ("body_len", ctypes.c_int32),
        ("ndim", ctypes.c_int32), ("dims", ctypes.c_int32 * 8),
        ("langtype", ctypes.c_char * 256), ("langtype_len", ctypes.c_int32),
        ("body_offset", ctypes.c_int32), ("body_cap", ctypes.c_uint64),
    ]

_U8P = ctypes.POINTER(ctypes.c_uint8)
_bind(_lib.kvspaceTlvEncodeMode,
      [ctypes.c_char_p, _U8P, ctypes.c_uint32, ctypes.POINTER(ctypes.c_int32), ctypes.c_int32,
       ctypes.c_int32, ctypes.c_uint8, ctypes.c_uint32, ctypes.POINTER(_U8P), ctypes.POINTER(ctypes.c_uint32)],
      ctypes.c_int)
_bind(_lib.kvspaceDecodeHead, [_U8P, ctypes.c_uint32, ctypes.POINTER(_Head)], ctypes.c_int)

REF_INLINE, REF_PTR, REF_EXT = 0, 1, 2


# ── XValue TLV helpers ──────────────────────────────────────────
# 编解码一律委托权威 C codec（kvspaceTlvEncodeMode / kvspaceDecodeHead），
# 本地不再复刻 wire 布局。

def _take(out, ol: ctypes.c_uint32) -> bytes:
    data = ctypes.string_at(out, ol.value)
    _libc.free(out)
    return data


def _xv_encode(kind: str, raw: bytes = b"", dims: tuple = (), ref: int = REF_INLINE) -> bytes:
    """编码 XValue head+body：绑定 C codec `kvspaceTlvEncodeMode`。
    dims 落形状（tensor），ref 选存储位置（inline/ptr/ext）。"""
    buf = (ctypes.c_uint8 * len(raw)).from_buffer_copy(raw) if raw else None
    d = (ctypes.c_int32 * len(dims))(*dims) if dims else None
    out = _U8P()
    ol = ctypes.c_uint32()
    rc = _lib.kvspaceTlvEncodeMode(kind.encode(), ctypes.cast(buf, _U8P) if buf is not None else None,
                                   len(raw), d, len(dims), ref, 0, 0,
                                   ctypes.byref(out), ctypes.byref(ol))
    if rc:
        raise ValueError(f"kvspaceTlvEncodeMode({kind!r}, ref={ref}) failed: {rc}")
    return _take(out, ol)


def _kx_base(lt: str) -> str:
    """剥掉纯数字 `[dims]` 段取 base kind；含 `·` 的 map langtype 整串即基 kind。"""
    if "·" in lt:
        return lt
    if lt.startswith("["):
        end = lt.find("]")
        if end > 0 and all(p == "" or p.lstrip("-").isdigit() for p in lt[1:end].split(",")):
            return lt[end + 1:]
    return lt


def _xv_decode(data: Optional[bytes]) -> tuple[str, int, bytes]:
    """解码 XValue head+body：绑定 C codec `kvspaceDecodeHead`。→ (kind, array_len, raw)"""
    if not data:
        return ("", 0, b"")
    buf = (ctypes.c_uint8 * len(data)).from_buffer_copy(data)
    h = _Head()
    if _lib.kvspaceDecodeHead(buf, len(data), ctypes.byref(h)):
        return ("", 0, b"")
    kind = _kx_base(h.langtype[:h.langtype_len].decode())
    dims = list(h.dims[:h.ndim])
    al = 1
    for d in dims:
        al *= d
    raw = bytes(data[h.body_offset:h.body_offset + h.body_len])
    return (kind, al if dims else 1, raw)


def xv_int(v: int) -> bytes:
    return _xv_encode("int64", struct.pack("<q", v))


def xv_float(v: float) -> bytes:
    return _xv_encode("float64", struct.pack("<d", v))


def xv_str(s: str) -> bytes:
    return _xv_encode("char/utf8", s.encode())


def xv_index() -> bytes:
    """目录值。旧 `index` 值类型已由「值/索引分离」移除，现行目录值 kind 为 `lib`。"""
    return _xv_encode("lib", b"")


def xv_link(target: str, kind: str = "lib") -> bytes:
    """指针值（ptr 位）：body=目标 key，kind=目标完整 kindexpr。"""
    return _xv_encode(kind, target.encode(), ref=REF_PTR)


def xv_ext(locator: str, kind: str = "lib") -> bytes:
    """外部定位符（EXT class）：body=locator，kind=目标完整 kindexpr。"""
    return _xv_encode(kind, locator.encode(), ref=REF_EXT)


def xv_map(langtype: str) -> bytes:
    """map 容器值：现行 wire 只存完整 map langtype `{key}·{value}`（成员不落正文）。"""
    if "·" not in langtype:
        raise ValueError("map langtype must contain '·'")
    return _xv_encode(langtype, b"")


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
