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

Fixed 8-byte head (`#566`) followed by the body and an attr region (`#571`), stored as one value under one key:

```
[len:u48 le @0-5][storetype:u8 @6][ptr:u8 @7][body @8][attr @8+len]
attr = [attr_len:u16 le][records]      attr_len includes itself; empty attr = 2
record = [tag:u8][len:u16 le][data]    tag 1 = langtype (kindexpr); 2+ reserved
total = 8 + len + attr_len
```

- storetype selects short, slack, large, or extension storage; ptr marks pointer/ext.
- langtype (kindexpr) is the tag-1 record of the attr region; no record = untyped.
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
