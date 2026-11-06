# kvspace-c

[![CI](https://github.com/array2d/kvspace-c/actions/workflows/ci.yml/badge.svg)](https://github.com/array2d/kvspace-c/actions/workflows/ci.yml)

C implementation of the **KVSpace** used by kvlang — the filesystem-style key-value store that serves as kvlang's unified addressing and memory space (keys are paths, values are XValues).

This is one of two standard implementations of the KVSpace contract; the other is [kvspace-durable](../kvspace-durable). Both expose the same C ABI and the same XValue kindexpr format, so a consumer (the kvlang layout/runtime) switches between them by DSN only.

Backend: `shm://` — single file-backed mmap block (ART-tree key index + slotsboxmalloc value storage), shared across processes.

## Build

```bash
make            # → build/libkvspace-c.so
make test       # → build-test/, runs ctest (needs the kvspace ABI headers)
```

Dependencies: [`blockmalloc`](../blockmalloc), [`slotsboxmalloc`](../slotsboxmalloc) (header-only, compiled into this library).
Tests additionally include the authoritative ABI header `<kvspace/kvspace.h>` from [`kvspace`](../kvspace) (`include/`).
`ci/deps.sh` installs all three at the versions pinned in `deps.json`.

## API

Two surfaces:

1. **Native C API** (`kvspaceShm*`) — `src/kvspace_shm.h` (internal header):
   `kvspaceShmOpen/Close`, `kvspaceShmGet/Set`, `kvspaceShmList`, `kvspaceShmDel/Deltree`, `kvspaceShmMkindex`, `kvspaceShmExtindex/Delextindex`, `kvspaceShmNotify/Watch`.

2. **durable C ABI** — byte-compatible with `kvspace-durable` (`src/durable_abi.c`):
   `kvspaceConnect/Close/Disconnect`, `kvspaceGet` (zero-copy borrow) `/WriteInPlace/WriteNewPlace`,
   `kvspaceListLen/ListAt/Del/DelTree/Cp/CpTree`,
   `kvspaceMkindex/MkindexExt/RmindexExt/Watch/Clear`,
   `kvspaceNewPtr/NewChar/NewBool/NewInt64/NewFloat64`.

   A consumer (e.g. the kvlang layout) links this ABI and switches backends by DSN only, with no code change.

## XValue

Fixed **16-byte** head followed by the body and an attr region, stored as one value under one key.
This section mirrors `kvspace/include/kvspace/xvalue_head.h` verbatim — that header is the
single source of truth for the wire layout.

```
[bodylen:u64 le @0-7][attrslen:u32 le @8-11][layout:u8 @12][padding:3 @13-15][body @16][attr @16+bodylen]
attr  = records, no length prefix      (attrslen is in the head — a second prefix would be a second truth)
record = [tag:u8][len:u16 le][data]    tag 1 = langtype (kindexpr); tag 2 = reallen; 3+ reserved
total = 16 + bodylen + attrslen
```

- **`bodylen` 的语义由 `layout` 决定**：`layout = 0`（LENGTH）时它就是有效长度；
  `layout = 1`（CAPACITY）时它是**容量**，有效长度在 attr tag 2（`reallen`）。
  `padding` 必须为零，非零即非法。
- **没有 storetype / ptr 字段**：存储形态与「是不是引用」都从 langtype 推出——
  langtype 首字节 `*` 是 Ptr、`@` 是 Ext，其余是普通值。
- langtype (kindexpr) is the tag-1 record of the attr region; no record = untyped.
- tag 2 的归属是**排他**的：`layout = CAPACITY` 必须恰有一条，其它 layout 必须不带。
- Values are built with the `kvspaceXvalueHeadNew*` constructors. `kvspaceSetValue`
  rejects values whose attr region is missing or runs past the value.
- Store format version 6 (this wire). Stores written by older versions are refused at open; rebuild them.

## Tests

```bash
make test                                                  # C tests (ctest)
KVSPACE_C_SO=build/libkvspace-c.so python3 py/tests/test_wire.py
(cd rust && cargo test)                                    # links ../build
```

Cross-language tutorials live in the [`kvspace`](../kvspace) repo (`tutorial-c/`).

## Language wrappers

- `py/` — Python ctypes binding.
- `rust/` — Rust FFI crate.
