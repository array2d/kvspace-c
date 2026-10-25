# kvspace

KVSpace 的 dispatch 前端。消费者只链接 `libkvspace`，运行期按 DSN scheme 选择后端：

| DSN scheme | 后端 | 装载名 |
|---|---|---|
| `shm://...` | kvspace-c（file-backed mmap + ART） | `libkvspace-c.so.1` |
| `redis://` `fs://` `s3://` | kvspace-durable（Rust） | `libkvspace_durable.so.1` |

两个后端导出同一套 `kvspace*` C ABI（见 `include/kvspace/kvspace.h`）。
前端用 `dlopen(RTLD_NOW | RTLD_LOCAL)` 装载后端，handle 包一层 vtable；
codec（`kvspaceXvalueEncode*`/`kvspaceDecodeHead`/`kvspaceNew*`）无 handle，由前端静态实现，
head 格式 byte-identical，两个后端与前端三者共用同一份契约。

后端查找目录由环境变量 `KVSPACE_BACKEND_PATH` 覆盖，默认走动态链接器搜索路径。

## 常量 ABI

`include/kvspace/const.h` 是分隔符与 kind 串的唯一定义处（`#define KVSPACE_*`），
并导出 `kvspaceConst("KVSPACE_KIND_MAP")` → `"stringkeymap"` 供 Go/Python/Rust 扩展运行期取常量，
免去硬编码裸字面量。C/C++ 直接 `#include <kvspace/const.h>` 用宏；Rust 用 build.rs 解析同一文件的
`#define KVSPACE_* "…"` 行生成常量，不引入 bindgen。

## Build

```bash
cmake -B build && cmake --build build   # 产出 libkvspace.so.1
```

## Install

```bash
cmake --install build                   # libkvspace.so.1 + include/ + kvspace.pc
```

## 目录

```
include/kvspace/kvspace.h   # 权威 C ABI 头（include const.h）
include/kvspace/const.h     # 分隔符/kind 常量唯一定义处 + kvspaceConst()
src/frontend.c              # dispatch 前端 + 静态 codec + kvspaceConst 查找表
cli/                        # kvspace CLI（从 kvspace-durable 迁入）
tutorial-c/                 # kvspace-c（SHM）多语言 tutorial
tutorial-durable/           # kvspace-durable（redis/fs/s3）tutorial
```

