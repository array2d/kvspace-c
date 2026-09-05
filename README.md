# kvspace-c

[![CI](https://github.com/array2d/kvspace-c/actions/workflows/ci.yml/badge.svg)](https://github.com/array2d/kvspace-c/actions/workflows/ci.yml)

C implementation of the **KVSpace** used by kvlang — the filesystem-style key-value store that serves as kvlang's unified addressing and memory space (keys are paths, values are XValues).

This is one of two standard implementations of the KVSpace contract; the other is [kvspace-durable](../kvspace-durable). Both expose the same C ABI and the same XValue kindexpr format, so a consumer (the kvlang layout/runtime) switches between them by DSN only.

Backend: `shm://` — single file-backed mmap block (ART-tree key index + slotsboxmalloc value storage), shared across processes.

## Build

```bash
make            # → build/libkvspace-c.so
```

Dependencies: [`blockmalloc`](../blockmalloc), [`slotsboxmalloc`](../slotsboxmalloc) (header-only + `.so` dual-mode libraries).

## API

Two surfaces:

1. **Native C API** (`kvspaceShm*`) — `include/kvspace/kvspace.h`:
   `kvspaceShmOpen/Close`, `kvspaceShmGet/Set`, `kvspaceShmList`, `kvspaceShmDel/Deltree`, `kvspaceShmMkindex`, `kvspaceShmExtindex/Delextindex`, `kvspaceShmNotify/Watch`.

2. **durable C ABI** — byte-compatible with `kvspace-durable` (`src/durable_abi.c`):
   `kvspaceConnect/Close/Disconnect`, `kvspaceGet` (zero-copy borrow) `/WriteInPlace/WriteNewPlace`,
   `kvspaceListLen/ListAt/Del/DelTree/Cp/CpTree`,
   `kvspaceMkindex/MkindexExt/RmindexExt/Watch/Clear`,
   `kvspaceTlvEncode/TlvEncodeMode/DecodeHead`,
   `kvspaceNewPtr/NewChar/NewBool/NewInt64/NewFloat64`.

   A consumer (e.g. the kvlang layout) links this ABI and switches backends by DSN only, with no code change.

## XValue

kindexpr TLV head, byte-identical to `kvspace-durable` (`include/kvspace/xvalue.h`):

```
[1B kindexprlen][kindexpr + 0x00 pad][1B ro][4B vid LE][4B raw_len LE][raw]
```

- kindexpr first byte: `*` = soft link (raw = target path), `@` = ext handle, otherwise inline.
- `[d0,d1]kind` carries ndim+dims; bare `kind` is a scalar; `char/*` is always a 1-D sequence (`[n]`).
- `None` is encoded as NULL / length 0.

Kinds: `bool`, `int8..int64`, `uint8..uint64`, `float32/64`, `char/utf32|utf8|ascii`, `objindex`, `strkeymapindex`, `index`, `extindex`, `rwir`, `rwfunc`, `defrwir`, `defrwfunc`, `scope`, `time`, `duration`.

## Tutorial

```bash
python3 tutorial/test.py
```

Cases: `01_basic.c`, `02_cpp.cpp`, `03_python.py`, `04_rust.rs`, `05_integrity.c`, `06_multiprocess.c`.

## Language wrappers

- `py/` — Python ctypes binding.
- `rust/` — Rust FFI crate.
