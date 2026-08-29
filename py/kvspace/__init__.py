"""
kvspace-c Python wrapper — ctypes FFI over libkvspace-c.so
"""

import ctypes, os, struct
from pathlib import Path
from typing import Optional

_SO = os.environ.get("KVSPACE_C_SO")
if not _SO:
    _SO = str(Path(__file__).parent.parent.parent / "build" / "libkvspace-c.so")
_lib = ctypes.CDLL(_SO)


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


# ── XValue TLV helpers ──────────────────────────────────────────

def _xv_encode(kind: str, raw: bytes, dims: tuple = (), ref: int = 0, ro: int = 0, vid: int = 0) -> bytes:
    kx = ("*" if ref == 1 else "@" if ref == 2 else "") + ("[" + ",".join(map(str, dims)) + "]" if dims else "") + kind
    kb = kx.encode()
    return struct.pack(f"<B{len(kb)}sxBII", len(kb) + 1, kb, ro, vid, len(raw)) + raw


def _xv_decode(data: Optional[bytes]) -> tuple[str, int, bytes]:
    if not data:
        return ("", 0, b"")
    slot = data[0]
    kx = data[1:1 + slot].split(b"\x00", 1)[0].decode()
    o = 1 + slot
    rl = struct.unpack_from("<I", data, o + 5)[0]
    raw = data[o + 9 : o + 9 + rl]
    if kx.startswith("*"):
        kx = kx[1:]
    elif kx.startswith("@"):
        kx = kx[1:]
    dims = []
    if kx.startswith("["):
        end = kx.index("]")
        dims = [int(d) for d in kx[1:end].split(",")]
        kx = kx[end + 1:]
    al = 1
    for d in dims:
        al *= d
    return (kx, al, raw)


def xv_int(v: int) -> bytes:
    return _xv_encode("int64", struct.pack("<q", v))


def xv_float(v: float) -> bytes:
    return _xv_encode("float64", struct.pack("<d", v))


def xv_str(s: str) -> bytes:
    b = s.encode()
    return _xv_encode("char/utf8", b, dims=(len(b),))


def xv_index() -> bytes:
    return _xv_encode("index", struct.pack("<I", 0))


def xv_link(target: str) -> bytes:
    return _xv_encode("index", target.encode(), ref=1)


def xv_ext(extpath: str) -> bytes:
    return _xv_encode("extindex", struct.pack("<I", 0) + ("…" + extpath).encode())


def xv_map(children: list[str], dims: tuple) -> bytes:
    """stringkeymap 散 key ndarray：body=[4B count LE][child\n...]，dims 落 head。"""
    body = struct.pack("<I", len(children)) + "\n".join(children).encode()
    return _xv_encode("stringkeymap", body, dims=dims)


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
