# kvspace-c（已冻结）

> **本仓冻结，不再更新。** SHM 存储引擎已并入 [`kvspace`](../kvspace) 仓。

## 实现去哪了

| 原位置 | 新位置 |
|---|---|
| `kvspace-c/src/`（`kvspace.c`、`coord.c/h`、`kvspace_shm.h`、`xvalue_internal.h`） | [`kvspace/src/shm/`](../kvspace/tree/main/src/shm) |
| `kvspace-c/tests/` | [`kvspace/tests/shm/`](../kvspace/tree/main/tests/shm) |
| `kvspace-c/CMakeLists.txt` 的构建逻辑 | `kvspace/CMakeLists.txt` 的 `kvspace-c` target |

## 怎么构建

在 `kvspace` 仓里，目标名仍是 `kvspace-c`：

```bash
cmake -S ../kvspace -B ../kvspace/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build ../kvspace/build --target kvspace-c -j
```

产物名与 SONAME **保持不变**：`libkvspace-c.so.1`。dispatch 前端按这个名字 `dlopen`
后端，改名就断链——所以并入后仍叫 `kvspace-c`，不叫 `kvspace-shm`。

测试：

```bash
cmake --build ../kvspace/build -j
cd ../kvspace/build && ctest --output-on-failure   # shm_* 开头的 8 项
```

## 为什么并仓

`kvspace-c` 只依赖 `kvspace` 的公开头（`kvspace.h`、`kvspace_xvalue.h`、
`kvlang_xvalue.h`）与两个 header-only 分配器（`blockmalloc`、`slotsboxmalloc`）。
两仓分离带来的全部代价是「找兄弟目录的头 + 配置期断言版本对不对」这一层
跨仓布局补丁，而没有换来任何独立性——真正的解耦早已由头文件完成。并仓后那层
补丁是纯冗余，故删除。

## 历史

本仓保留是为了让旧的 git 历史与 issue 引用仍可追溯，**代码不再演进**。
