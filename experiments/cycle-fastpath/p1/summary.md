# P1 报告 — 无新 ABI 的内部压缩

> 口径与 P0 完全一致（bj-build、rdtscp+标定、taskset、setenv -R、单热 key、8B、3×30×20k）。
> 变体是**累积**的：p1a → p1b → p1c。原始数据 `p1/results/*.csv`，评审 patch
> `patches/kvspace-c-p1.patch`（107 行，3 个文件）。**未直接改仓库工作区。**

## 1. 结果（realistic key `/vthread/123456/[12]/t0`，ticks / core cycles）

| 配置 | P0 | p1a 去 1023B 拷贝 | p1b +免 size 查询 | p1c +跳过全量校验 |
|---|---:|---:|---:|---:|
| 根路径 Get 命中 | 423.0 / 535 | 386.4 / 489 | 269.4 / 341 | **229.8 / 292** |
| **prefix+rel Get** | 169.0 / 214 | 171.7 / 217 | 139.9 / 177 | **99.5 / 126** |
| Get 未命中 | 406.0 / 513 | 368.1 / 465 | 357.1 / 451 | 355.3 / 451 |
| Set 覆盖 | 506.0 / 642 | 447.8 / 566 | 507.0 / 641 | 468.5 / 594 |
| Del 存在 | 408.3 / 516 | 367.4 / 464 | 399.7 / 505 | 371.7 / 472 |
| short Get 命中 | 383.4 / 487 | 347.9 / 440 | 249.6 / 315 | 207.2 / 263 |
| Set 新建 | 1342 / 1697 | — | — | 1315 / 1668 |

**逐项 delta（realistic，cycles）**
- ① 定长拷贝 → strnlen+memcpy：根 Get **−46**（535→489）；prefix 路径无拷贝，**0**。
- ② 读路径免 `sbo_allocated_size`：根 Get **−148**（489→341）；prefix Get **−40**（217→177）。
- ③ 读路径跳过 `xh_validate`（写入仍全量校验）：根 Get **−49**（341→292）；prefix **−51**（177→126）。
- ④ wire 头解码缓存：**未实现**。p1b→p1c 的 −49/−51 已经把“全量校验”这部分拿掉；剩下的是
  只解析头（`xh_header`）+ 边界检查，量级已很小（见 §3）。先做解码缓存只会缓存一个几 cycle 的
  操作，**建议并入 P2 的 leaf ref 一起做**，本轮不做。

## 2. 达标判定

| 验收项 | 目标 | 实测 | 结论 |
|---|---|---:|---|
| prefix+rel Get（realistic） | ≤100 cyc | **126** | **未达标（+26%）** |
| 根路径 Get（realistic） | ≤250 cyc | **292** | **未达标（+17%）** |
| 字符串 ABI 回归 | ≤5% | 所有操作**更快**（Get −45%、Set −7.5%、Del −10%、miss −12%） | 通过 |
| set_new | ≤300 cyc（P0 定的建议目标） | 1668 | 本阶段只降 1.7%；P1 不覆盖分配路径 |

**为什么没到**：126/292 里仍包含
- ABI dispatch ≈11 cyc（p1c 测的是字符串 ABI；prefix 探针不含 dispatch，但有一次跨 DSO 探针调用 ≈6–10 cyc，属上界）；
- ART 走查：prefix 路径只剩 2 字节（`t0`）+ 一次 `art_child`；根路径要走 23 字节；
- `read_tlv` 的头解析与边界检查（省掉了校验，但解析仍在）；
- `kv_sync`/指针链等固定开销。

这些正是 §3.1.1 的 ref/prefix 对象要消掉的项（P2：ref 缓存 leaf/slot，免每读头解析与 ART 走查）。
P1 已把“每次读都做的重活”（1023B 拷贝、shard 锁、UTF-8 全量扫描）清掉，P1 的定位即此。

## 3. 头解析还剩多少（用于判断第 4 项）

- P0 里 `kvspaceXhView`（头解析 + 全量校验）的直接调用成本 ≈ 88 ticks / **111 cycles**
  （cycle-audit `probe_api`，8B 值）。
- p1b→p1c 去掉全量校验，根 Get 降 **49 cycles**、prefix Get 降 **51 cycles**。
- 因此 p1c 读路径剩下的“头解析 + 边界检查”大致是 111 − 49 ≈ **60 cycles 量级**；但两次测量
  的工作集不同（直接调用 vs Get 内联），**这个差只能当上界，本轮没有单独测量 head-only 的
  直接成本**。
- 结论：第 4 项（解码缓存）能省的上限就是这几十 cycles，而且要新增缓存与失效逻辑；**建议并入
  P2 的 leaf ref**（ref 本来就要缓存 value_len/type），本轮不单独做，避免为几十 cycles 引入
  一套新的失效面。

## 4. 代码位置（pinned v0.3.14）

| 项 | 位置 |
|---|---|
| ① 定长拷贝 | `kvspace-c/src/kvspace.c:1423`（Get）、`:1770`（WriteInPlace）、`:1858`（Del）、`:1354`（resolve_fetch）、`:1322`（resolve_path） |
| ② 免 size 查询 | `kvspace.c:1302`（read_tlv）、`:1362`（resolve_fetch）→ `slotsboxmalloc/include/slotsboxmalloc/slotsboxobj.h:709/669`（shard 锁 + 树下降） |
| ③ 跳过全量校验 | `xvalue_head.c:595`（kvspaceXhView）→ 新增 `kvspaceXhHeadView`（只 `xh_header`）；读路径 `kvspace.c:1305/1360` 改用它并加 `total <= sz` |
| ④ 解码缓存 | 未实现（见 §1/§3） |

## 5. 产物与哈希

- `patches/kvspace-c-p1.patch`（sha256 前 16 位 `8f7da3a0083b1fc6`），含 kvspace.c / xvalue_head.c / xvalue_head.h
- 变体 DSO：p1a `d236ab4f859cee7f`、p1b `15364d1261431ab2`、p1c `660ff9d5d247f31a`
- 原始 CSV：`p1/results/`（19 个配置 × 3 运行 × 30 批）

## 6. 未做 / 风险

- ③ 的语义前提：**所有写入都走校验路径**（`kvspaceShmSetValue` 内部 `kvspaceXhDecode` 全量校验）。
  直接 mmap 篡改不在保证范围（与 design §3.3 一致）。这条需要安全评审确认。
- ② 用数据区上界替代精确 box 大小，放松了 wire 视图的精确长度检查（但仍检查 `total <= sz`）。
- ③ 没有实现“值级 gen”（那属于 P2 的 leaf ref）。
- set_new 未受益（分配与删除路径未动）。

## 7. 回归（228 锚定用例）

| 后端 | P1 后端 | 未打补丁对照 | 结论 |
|---|---|---|---|
| shm | **227/228**（失败 `14-networld/04-kvlang.kv`） | 227/228（同一用例） | 无 P1 引入的回归 |
| fs | **225/228**（`10-types/06`、`14-networld/04`、`14-networld/05`） | 225/228（同三条） | 无 P1 引入的回归（这三条是 durable 既有问题） |
| redis | **未完成** | — | 本机复现失败：单例 `01-basics/hello` 也失败；且 **redis 走 durable 后端，完全不经过 kvspace-c**，因此它检验不了本 patch |

- 回归使用 macOS 本机 kvlang + P1 的 kvspace-c `dylib`（v0.3.14 基线），对照组用未打补丁的
  v0.3.14。两组失败清单逐条相同。
- redis 失败原因未深究：疑似本机 kvlang HEAD（e9ceecc）与 `build/abi` 的 v0.3.0 bundle 不匹配，
  属环境问题；与 P1 无关。
