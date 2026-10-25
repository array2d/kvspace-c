# P2 原型实测（v0.3.16 沙箱树，未落地）

生成器 `scripts/make_p2.py`（gen 旁路表 + token API + FromRef + head-only）+ `scripts/make_p2_abi.py`
（规范头 + 前端 vtable/trampoline）。所有数字为 **公共 ABI**（`kvspaceConnect` + 前端 dispatch），
非直连；3 运行 × 30 批 × 20000 op，校准/taskset/关 ASLR 同前。

## 1. 达标表（ABI 口径，cycles）

| 操作 | realistic 23B | short 16B | 目标 | 结论 |
|---|---:|---:|---:|---|
| **Get**（ViewCheck + 读 body） | **21.9** | **21.9** | ≤50 | ✅ |
| **Set 覆盖**（ViewWrite：check+2×gen+8B 写） | **43.8** | **43.8** | ≤50 | ✅ |
| **ResolveFrom(prefix, rel)**（ResolvePrefix 入口） | **184.6** | **147.0** | ≤300 | ✅ |
| Resolve root（合并入口） | 462.9 | 385.0 | 单独报 | 记录 |
| FromRef（已解析 ref → token） | 134.5 | 132.7 | — | 记录 |
| Del（含 256-epoch 回收） | 275.1 | 274.2 | 前台 ≤50² | ❌/待定² |

¹ 已修：新增 `kvspaceViewResolvePrefix` 显式解析目录前缀（允许边界落在节点压缩前缀内部），
short 键 `depth==klen` 不再是问题；两档都 ≤300（184.6 / 147.0 cycles）。
² **Del（两阶段 harness：先建结构、再解析 token；计时窗口只含 ViewDel）**

| 配置 | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| 大队列（65536，前台无回收） | **131 cyc** | 197 | 230 | 6,972 |
| 256 队列（满时同步 flush） | 131 cyc | **920** | **1,150** | **97,000** |

- **前台 p50 131 cycles，仍 >50**。构成：前端 dispatch + **两次原子 gen**（value+struct，
  各 ~25–30 cyc）+ tomb 队列 append（512KB 数组，冷行）+ art_hdr/has_value。可压缩项：
  删除只 bump `struct_gen`（value_gen 对删除冗余）、ring 缩小到 L1（4KB）、gen 更新合并。
- **满队列尾路径不可接受**：256 队列时 p95 920 / p99 1150 / max 97k cycles。按裁决必须做
  **backpressure/spill**；正确形态是**后台 flush 线程**（队列只为前台入队，满时前台等待或
  丢弃旧批次），不能在调用线程里同步 `sbo_free`。
- token 失效行为符合模型（插入兄弟键 → `struct_gen` 变 → STALE）；“插入后 STALE+refresh
  摊销”这一项**尚未单独测量**，等 backpressure 方案定了与多进程测试一起做。
- **本轮不宣称 Del 达标，patch 不冻结。**

## 2.6 A/B 两种“前台不再 append 大队列”方案

| 方案 | 实现状态 | 前台成本 | 结论 |
|---|---|---|---|
| A：后台 flush 线程 + L1 ring（256×8B） | 已实现，**del 路径崩溃（未定位）**，无有效数字 | 预期 ≈ 单次 store + tail++ | 需要再一轮调试（线程与 allocator 的并发点） |
| B：slot 元数据 pending-free 标志 + allocator 周期 sweep | **未实现** | 预期 ≈ 一次标志位写 | slotsbox 每槽只有 1B `states[]`（编码 start/len），没有空位；要在 `states` 编码里加 pending 位或加外部位图，改动在 allocator 侧，面比 A 大 |

**已测到的 floor（上一版可用构建）**：前台 p50 **131 cycles**（大队列，无回收），p95 197；
256 同步 flush 时 p95 920 / p99 1150 / max 97k。

**gen 降本**：本次构建已把 Del 改成只 bump `struct_gen`（普通自增，单写者模式），
但同一构建被 A 的崩溃阻断，**这项数字未单独测出**；按两项原子合并估算，约省 25–30 cyc，
即 floor 约 100–105 cycles，**仍 >50**。

**结论（触底）**：在“前台入队 + 后台回收”的形态下，Del 前台 floor 约 **100–131 cycles**，
到 50 需要 B（无队列、allocator sweep）或进一步合并 dispatch/gen；建议由你与用户另定
Del 的独立目标，不在本轮硬凑。

## 2. 结论

1. **Get/Set-over 在 ABI 口径下已达标**（21.9 / 43.8 cycles，目标 50）。旁路 gen 表 + seqlock
   的额外成本 1–2 cycles，没有把 per-op 推过线（裁决①的门槛问题已用实测回答）。
2. **ResolveFrom 227.8 cycles ≤ 300**：新入口 `kvspaceViewFromRef`（已解析 ref 直接出 view）
   + head-only value_len 把合并 Resolve 从 894 压到 228；root resolve（462.9/385）按裁决单独报，
   不计入 300。
3. **Del 只有“含回收”的 275，纯前台数未收敛**（见 ²）。按裁决③的语义，Del 的 50c 只应覆盖
   tombstone+gen+入队；后台回收摊销 150–170 cycles/删——这组数字等重测后补。
4. root Resolve 462.9 > 400 是**单入口的 root 解析**，按裁决不计入 ResolveFrom 目标；若后续
   要求 root 也 ≤300，需另做（SlotView 已不再重复走查，剩下的主要是 ResolveRef 自身 231 +
   前端 dispatch + ViewFromRef 的 cap/head 读取）。

## 2.5 前端对比（沙箱 vs 真实前端）

同一后端、同一字符串 ABI 操作，只换前端库（stock v0.3.16 vs 打了 P2 ABI 的前端）：

| 操作 | stock 前端 | patched 前端 | 差 |
|---|---:|---:|---:|
| Get 命中 | 493.6 cyc | 431.3 cyc | −12.6%¹ |
| Set 覆盖 | 690.3 cyc | 706.1 cyc | **+2.3%** |

¹ 方向相反且量级偏大，判断为会话间/批次噪声；关键是 **Set-over 只差 +2.3%**，说明新增
vtable 字段与 dlsym 没有引入系统性 dispatch 开销。token 三个数（21.9/43.8/184.6）只能在
patched 前端上测（stock 没有这些符号），上表作为“前端本身没有变慢”的旁证。

## 3. 未交付

- 四仓 patch 未拆分（Del 前台数与多进程测试未完成，暂不冻结 patch）。
- 多进程 STALE / store 重开 / 256-epoch 并发安全未测（裁决⑤：Resolve 达标后补——Resolve
  已达标，但 Del 未定，建议与 Del 重测一起做）。
- P1 ①②③ 的 rebase 未做。

## 2.7 A 的 SIGSEGV 根因与修正（并发正确性）

- **栈**（ASan）：`kv_sbo_free_deferred` @ kvspace.c:625，`WRITE to 0x0`；调用链
  `kvspaceShmViewDel → kv_sbo_free_deferred`。
- **根因**：生成器里“open 时分配 tomb ring + 启动 flusher”的那次替换**静默锚点不匹配**，
  源码里只有 close/fail 路径引用了 `tomb_ring`，分配从未发生 → 首次延迟释放写 NULL。
  （不是线程竞争，是初始化缺失。）
- **修正**：生成器改用带断言的 `sub()` 插入 `calloc + pthread_create`；ASan 复跑不再崩溃。
  **教训**：生成器里所有替换都已改为断言式（此前 `str.replace` 静默失败过一次）。

## 2.8 Del 前台 floor（干净构建，单写者：Del 只 bump struct_gen）

| 配置 | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| 大 ring（无窗口内回收） | **131 cyc（104 ticks）** | 164 | 197 | 6.6–8k |
| 16/64/256/1024 token 工作集（同上） | 131 | 131–164 | 197–230 | — |
| 256 ring + 后台 flusher（阻塞 backpressure） | **18,600 cyc** ❌ | — | — | — |

**关键发现（设计级）**：后台 flusher 的回收速率（≈150 cyc/box）**低于**前台删除速率
（≈105 cyc/op），1:1 回收永远追不上，ring 必满 → 前台自旋。**后台线程只有在“突发之间
排空”时才有意义**（ring 容量 ≥ 单次突发），或回收必须批量化。这不是调参能解决的。

**对冻结目标（Del median ≤120、p95 ≤150）**：干净 floor = **131 / 164**，差 11 / 14 cycles。
可尝试：ring 回到 L1（256）且仅在突发后排空；去掉一次 `art_hdr()`；前端 trampoline 直接
调后端符号。**当前如实报：未达 ≤120。**

## 2.9 叶子级逻辑墓碑（当前实现，**达标**）

前台 Del = `has_value=0` + `value_gen++`（单写者，release fence）；不收 slot、不 bump
struct_gen、不入 ring。回收由后台 `kvspaceViewSweep`（DFS 扫墓碑叶，释放 box 并 bump
struct_gen）在窗口外完成。

| 配置 | p50 | p95 | p99 | max |
|---|---:|---:|---:|---:|
| K=64（3 run × 10 批） | **78 ticks / 98.6 cyc** | 78 / 98.6 | 104 / 131 | 1,326 / 1,676 |
| K=4096（3 run × 10 批） | **78 ticks / 98.6 cyc** | 104 / **131** | 130 / 164 | 30,290 / 38,287 |

**对冻结目标（median ≤120、p95 ≤150）：达标**（98.6 / 131）。max 是首触/扫掠干扰的离群，
如实列在表里，不剔除。

**语义核对（代码层）**
- Get：`art_walk` 在 `d==klen` 返回 `has_value ? nid : -1` → 墓碑叶即 miss ✓。
- List/ListLen：`art_has_live_value` 对 `has_value==0` 且无活子节点的叶返回 0 → 跳过 ✓。
- 重插入：`art_ins` 命中已有节点（has_value=0）→ 置值复活；harness 每批重设+重解析已验证 ✓。
- 持 token 读者：`value_gen++` 使它 STALE ✓；`struct_gen` 不变，所以**前缀 token 不受删除影响**。
- 堆积：墓碑叶在 sweep 前一直占用 box；`kvspaceViewSweep` 是全树 DFS，调用时机由后台决定
  （建议按“墓碑数阈值/时间片”触发，本轮用批间显式 sweep 验证）。
- 持续删除速率仍受回收速率限制（≈150 cyc/box 的 `sbo_free`）：≤120 的延迟目标是
  “突发之间能排空”的前提；持续高删会导致墓碑堆积或 sweep 跟不上，这部分 CPU 单列。

## 2.10 ResolveFrom 的 231/195 vs 184/147：来源已定位（不是噪声）

真实构建三种 flags 对比（同 v0.3.16 + 同 patch，公共 ABI，3×10 批）：

| 构建 flags | realistic | short |
|---|---:|---:|
| 仓库默认 Release（-O3 -DNDEBUG） | 231.1 cyc | 195.0 |
| `-O2 -g`（覆盖 CMAKE_C_FLAGS） | 231.3 | 195.6 |
| `-O2 -g -falign-functions=64 -falign-loops=64` | **195.0** | **148.4** |

结论：差异来自**代码布局/对齐**（-O2 与 -O3 无差；加上对齐 flag 后回到沙箱水平），
与 cycle-audit 里发现的布局敏感性一致。**仓库默认 flags 的真实数字是 231/195**（仍 ≤300），
报告以它为准；沙箱的 184/147 只作迭代参考。

## 2.11 ③ 回收协议：设计完成，未实现

见 `p2-reclaim-model.md`：按 view 引用计数（token 创建/释放各一次原子），sweep 只释放
`refcount==0` 的墓碑；含内存序表、无 UAF 论证、崩溃泄漏风险、TSan/ASan/多进程/重开测试计划。
**当前 patch 的 sweep 仍是“bump gen 后立即 free”，不具备并发安全性，patch 不冻结。**

## 2.12 ③ 模型修订（应裁决）

- **pin 改为单字 64-bit CAS**：`{gen:32 | ref:28 | pending:1 | dead:1 | flags:2}`，CAS 同时
  校验 gen 与 ref++；unpin 同 CAS。ABA 论证：节点 id 复用时 gen 取更大全局 epoch，CAS 必败；
  `gen:32` 回绕为理论边界（2^32 次分配）。
- **孤儿 pin 有方案**（不再是“本轮不实现”）：进程注册表（pid + starttime）+ 恢复屏障
  （`recovery_epoch` → 存活进程 ack → 提升 ref_base 作废旧 pin → 回收墓碑）；死进程不可能
  再访问 shm，作废其 pin 无 UAF；挂起进程有超时与审计日志。测试必须含 `kill -9` 孤儿回收。
- ViewRelease 由 runtime 帧缓存管理（帧销毁/STALE/evict 释放）。
- ResolveFrom 预计 +25 cyc（一次 pin CAS）→ 256，仍 ≤300，需实测。

## 2.13 对齐 flag 的结论（写进报告）

默认 Release 的 **231/195 cycles 是验收数字**；`-falign-functions/loops=64` 的 195/148 只作
说明，不作为默认构建结论。热路径函数（`kvspaceShmViewFromRef/Write/Del/Check`）建议显式
`__attribute__((aligned(64)))`，但需单独测量收益与副作用后再决定。
