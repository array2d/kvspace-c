# Rebaseline v0.3.16 — 现有 ref/slotview API 与 P1 处置

> 依据：用户规则「必须基于最新版本开发」。kvspace-c `486e67e == v0.3.16`、kvspace
> `cc88806 == v0.3.16`、blockmalloc `43f57a6 == v0.1.5`、slotsboxmalloc `b871d48 == v0.1.7`。
> 口径与 P0 相同（bj-build、rdtscp+标定、taskset、关 ASLR、单热 key、8B、3×30×20k）。
> 原始数据 `rebaseline/parts/`。**P0 的 v0.3.14 数字只作对照，不作结论。**

## 1. 字符串 ABI：v0.3.14 → v0.3.16（ticks / core cycles）

| 操作 | realistic 23B (v0.3.14) | realistic 23B (v0.3.16) | Δ |
|---|---:|---:|---:|
| Get 命中 | 423.0 / 535 | **383.9 / 485** | −9.3% |
| Get 未命中 | 406.0 / 513 | 383.7 / 487 | −5.5% |
| Set 覆盖 | 506.0 / 642 | 513.3 / 651 | +1.4% |
| Del 存在 | 408.3 / 516 | 399.8 / 507 | −2.1% |
| Set 新建 | 1342 / 1697 | 1379 / 1743 | +2.7% |

short 16B：Get 383.4/487 → **359.4/456**（−6.3%）。v0.3.16 的字符串 Get 有约 9% 的
自然改善（上游 TLV 改动等），但**仍距 50 cycles 有 10 倍**。

## 2. 现有 ref / slotview API（v0.3.16，realistic key）

| 调用 | ticks/op | core cycles | 说明 |
|---|---:|---:|---|
| `kvspaceResolveRef`（每次） | 182.4 | **231** | 一次解析：ART 走查 + parent/depth |
| `kvspaceGetByRef`（缓存 leaf ref，gen=0） | 194.0 | **246** | 直取叶子；仍含 dispatch+kv_sync+`read_value` 全量校验 |
| `kvspaceGetByRef`（parent+depth 前缀模式） | 260.7 | 331 | 只走 rel 的 2 字节，但比 leaf ref 更慢 |
| `kvspaceSlotView`（每次调用） | 328.2 | 416 | 每 op 重新取视图 |
| **`kvspaceSlotView` 一次 + 直接读/写 body** | **1.8** | **2** | 纯内存访问，**已远低于 50c 目标** |
| `kvspaceSetValueByRef`（缓存 ref） | 413.3 | 524 | 整值写 |
| `kvspaceSetPartByRef`（offset=headlen，8B） | 163.5 | 207 | 走的是回退路径（见 §4） |

**结论**：
- 现有 API 里**已经存在一条 2 cycles 的热路径**：`ResolveRef` + `SlotView` 各做一次，
  之后每 op 直读/直写 `view.body`。它的代价不在热路径，而在**失效语义**（借用指针，
  寿命到该 key 被写/删或 handle close；无 gen、无 store identity）。
- `GetByRef` 246 cycles 与 50c 的差距**不来自查找**（leaf 直取），而来自每次调用的
  dispatch + `kv_sync` + `read_value`（`sbo_allocated_size` + 全量 `kvspaceXhView` 校验）。
  也就是说 P1 的三项正是它的主要成本。

## 3. `parent_id/depth` 是否满足「可缓存前缀对象」

**部分满足，且有一个硬缺陷**：

| 要求 | 现状 |
|---|---|
| 定位目录前缀、免走前缀段 | ✅ real 键可用（depth=21 < klen=23），只走剩余 2 字节 |
| 短键 | ❌ **失败**：`/vthread/1/[1]/a` 解析出 depth=16 == klen，`ref_leaf` 的 `d >= klen` 直接返回 -1，前缀模式不可用 |
| 结构级 gen / STALE | ❌ 没有。`ref->gen` 被复用为 depth；节点搬迁只靠 `art_follow` 追 MOVED（≤8 跳），节点被删除后 id 复用无法检测 |
| store identity | ❌ `kvspaceRef_t` 只有 4 个 u32，无 epoch/UUID；跨 store 的 ref 无法识别 |

差异清单（只记录，不设计新 ABI）：
1. 需要「目录节点 + 已消费字节数」在 depth==klen 时仍可用的表示（当前直接失败）。
2. 需要结构级 gen（节点搬迁/子集变化递增），与 leaf 的值级 gen 分开。
3. 需要 store identity（epoch/UUID）以拒绝跨 store 的旧 ref。
4. 需要 `prefix + rel` 形态的入口（当前只能传完整 key + depth，rel 不能独立给出）。

## 4. P1 patch 是否被上游覆盖

| P1 项 | v0.3.16 现状 | 处置 |
|---|---|---|
| ① 1023B 定长拷贝 | **仍在**：`strncpy(kbuf,key,1023)` 3 处 + `resolve_fetch/resolve_path` 2 处 | **保留，rebase 到 v0.3.16** |
| ② 读路径免 `sbo_allocated_size` | **仍在**：`read_value`（kvspace.c:1302）每次读都调 | **保留，rebase** |
| ③ 读路径跳全量校验 | **仍在**：`read_value` 调 `kvspaceXhView`（含 `slack_count` 全量扫描） | **保留，rebase**；上游新增的 `SetPartByRef` SLACK 快路径只是局部先例 |
| ④ 解码缓存 | 上游用 `SlotView` 直接给 head/body 指针，缓存价值已转移 | **废弃**，并入 P2 的 ref+view |

**注意**：上游把 `read_tlv` 改名为 `read_value`，语义也有变化（空 langtype 的 Ptr/Ext
特例）。P1 的 patch 必须按 `read_value` 重新落点，不能直接 `git apply`。

## 5. 对 P2 的影响（只记录，不动手）

- 建议 P2 直接复用 `SlotView` 作为「ref + 缓存视图」的载体，而不是另造结构；
  缺的是 **失效语义**（§3 的差异清单 1–3）。
- 50c 目标的现实路径：`ResolveRef`+`SlotView` 每帧/每槽各一次（231+416 cycles 摊销到
  帧内几十到几百次操作），热路径 2 cycles；或者修好 `GetByRef` 的每调用开销。
- `set_new` 在 v0.3.16 没有改善（1743 cyc），仍受分配器与 punch 支配。

## 6. 环境

kernel 5.15.0-113 / EPYC 7K62 / 2 vCPU / gcc 11.2 / `-O2 -g -falign-functions=64
-falign-loops=64` / `taskset -c 0` / `setarch -R` / 标定 ≈1.264 cycles/tick。
DSO sha256 前 16：libkvspace-c `38b6e7a28d92daf3`、libkvspace `104522283cd9d4d6`。
