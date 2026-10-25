# P2 token ABI — patch 集（未合并，供评审）

基于 **v0.3.16**：kvspace-c `486e67e`、kvspace `cc88806`、kvspace-durable `342e7cd`、
blockmalloc `43f57a6`、slotsboxmalloc `b871d48`。全部用仓库自带 CMake（Release 默认 flags）验证过。

## 应用方式

```bash
git clone --depth 1 -b v0.3.16 https://github.com/array2d/kvspace-c.git
git clone --depth 1 -b v0.3.16 https://github.com/array2d/kvspace.git
git clone --depth 1 -b v0.3.16 https://github.com/array2d/kvspace-durable.git
cd kvspace-c && patch -p4 -s < ../kvspace-c.patch
cd kvspace   && patch -p4 -s < ../kvspace.patch
cd kvspace-durable && patch -p1 -s < ../kvspace-durable.patch
```

## 各仓内容

| 仓 | 文件 | 行数 | 内容 |
|---|---|---|---|
| **kvspace**（规范 ABI） | `include/kvspace/kvspace.h`、`src/frontend.c` | +121 | `kvspaceViewToken_t` 结构；`kvspaceViewResolve/FromRef/ResolveFrom/ResolvePrefix/Check/Write/Del/Refresh/Sweep` 原型；前端 vtable 字段（可选 dlsym，缺失返回 -2）与 trampoline |
| **kvspace-c**（shm 后端） | `src/kvspace.c`、`src/kvspace_shm.h`、`src/xvalue_head.{c,h}`、`src/durable_abi.c` | +704 | `<path>.gen` 旁路 gen 表（8B/节点，id 复用用全局 epoch）；gen 递增点（alloc/add/resize/值写/值删）；`kvspaceView*` 实现（FromRef 一次走查 + head-only 解码）；`kvspaceViewResolvePrefix`；叶子级墓碑 Del + DFS sweep；ABI wrappers |
| **kvspace-durable**（redis/fs） | `src/ffi.rs` | +33 | 9 个 `kvspaceView*` 桩，全部返回 **-2（ENOTSUP）**；调用方回退字符串 ABI。**未编译验证**（bj-build 无 cargo） |
| **kvlang runtime** | — | — | 接入方案见 `p2-runtime-plan.md`（未实现） |

## 实测（真实仓库构建，公共 ABI，3×10 批）

| 操作 | p50 | p95 | 冻结目标 | 结论 |
|---|---:|---:|---:|---|
| Get（realistic / short） | **21.9 cyc** | — | ≤50 / p95 ≤60 | ✅（p95 待补） |
| Set 覆盖 | **43.8 cyc** | — | ≤50 / p95 ≤60 | ✅（p95 待补） |
| ResolveFrom | **231.1 / 195.0 cyc** | — | ≤300 | ✅ |
| Del（叶子墓碑） | **98.6 cyc** | **131.5** | ≤120 / ≤150 | ✅ |

DSO sha256 前缀：`09bba0dcc3b63e6f`（kvspace-c，真实构建）。

## 已知未完成（**合并前必须解决**）

1. **sweep 并发安全未实现**：当前 sweep 在 bump `struct_gen` 后**立即** `sbo_free`，
   没有 grace period；持 token 的并发读者可能 UAF。需要 epoch/引用计数 + 内存序证明，
   以及 ASan/TSan + 多进程测试（architect ③）。
2. **P1 ①②③ 未 rebase**（1023B 拷贝 / `sbo_allocated_size` / 读路径全量校验）。
3. **runtime 接入方案**未写（`p2-runtime-plan.md` 待补）。
4. Del p95 只测了 K=4096 的批内样本；持续删除吞吐与墓碑堆积阈值未系统化。
5. **macOS 语义**：不做墓碑回收（无可证明的独占锁），`ViewDel` 只写墓碑，box 延迟到
   `kvspaceClose` 释放；Linux 才有 sweep + 恢复屏障。不要在 macOS 上引用本 patch 的
   回收语义。
