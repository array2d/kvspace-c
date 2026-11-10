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
[bodylen:u64 le @0-7][attrslen:u16 le @8-9][padding:6 @10-15]
[body @16][attr @16+bodylen]
attr  = records, no length prefix      (attrslen is in the head — a second prefix would be a second truth)
record = [tag:u8][len:u16 le][data]    tag 1 = langtype (kindexpr); tags 3+ reserved
total = 16 + bodylen + attrslen       (attrslen <= UINT16_MAX; padding must be zero)
```

- **`bodylen` 恒为有效字节数**，没有第二个长度来源。容量是存储后端的物理分配属性
  （`WriteNewPlace` 的 `body_cap` / box 尺寸），不进入 wire。10-15 六个字节只是
  padding，必须为零，非零即非法。
- **没有 storetype / ptr / layout 字段**：存储形态与「是不是引用」都从 langtype 推出——
  langtype 首字节 `*` 是 Ptr、`@` 是 Ext，其余是普通值。
- langtype (kindexpr) is the tag-1 record of the attr region; no record = untyped.
- **没有 tag 2**：早期 wire 的 `reallen`（容量形态的有效长度）已删除，出现即拒
  ——`kvspaceXvalueHeadScan` 是唯一的拒绝闸口，`EffectiveLen` 直接返回 `bodylen`。
- Values are built with the `kvspaceXvalueHeadNew*` constructors (no capacity
  argument: `Reserve(body_len, langtype)` 只产 `body_len` 字节零 body）。`kvspaceSetValue`
  rejects values whose attr region is missing, runs past the value, or carries tag 2.
- Store format version 8 (this wire). Stores written by older versions are refused at
  open; rebuild them. Version 7 wrote `layout = 1` into wire byte 12, which is padding
  now — it is rejected as nonzero padding, which is why the bump cannot be avoided.

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
