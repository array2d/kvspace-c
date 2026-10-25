# P2 回收协议（③ 设计；**未实现**）

## 1. 模型：按 view 引用计数（主选）

**为什么不用纯 epoch**：epoch 需要写者在读侧进入/退出保护期，读者热路径要维护线程本地
epoch；而 token 本身是调用方持有、生命周期明确的对象，**pin 在 token 上**最自然：
热路径（Get/Set/Del）不加 refcount，只在 **token 创建 / 释放**时各付一次原子操作。

### 元数据：单字 64-bit CAS（**不是分离的两次原子**）

`.gen` 每节点从 8B 扩到 **8B 的单个原子字**（外加 value_gen 仍单独 32-bit）：

```
packed64 = { gen:32 | ref:28 | pending:1 | dead:1 | flags:2 }
value_gen: u32        /* 值级 seqlock，单独字，读者只读它 */
```
- `gen`：节点分配时取全局单调 epoch（已有），**节点 id 复用时 gen 必不同**。
- `ref`：pin 计数（同进程 + 跨进程）。
- `pending`：墓碑且仍有 pin，等待最后一个 unpin 释放。
- `dead`：sweep 已宣布物理释放（防止重复释放）。

**pin（CAS，acq_rel）——必须同时拒绝 DEAD 与 `PENDING && REF==0`**：
```
for (;;) {
  w = load(packed, acquire);
  if (GEN(w) != tok->gen) return STALE;
  if (DEAD(w)) return STALE;                      /* 已物理释放 */
  if (PENDING(w) && REF(w) == 0) return STALE;    /* 已被 sweep 接管，等待回收 */
  if (CAS(&packed, w, w + REF(1))) break;         /* 校验 gen 与 ref++ 原子完成 */
}
```
（若只校验 gen，sweep 置 pending/dead 之后仍可被 pin，会把已回收的 box 重新挂上。）

**unpin 与 sweep 的 free 归属（恰好一方 free，DEAD 位是仲裁）**：
```
/* unpin */
for (;;) { w = load(acq); if (REF(w)==0) return;
  nw = w - REF(1);
  if (CAS(&packed, w, nw)) { w = nw; break; } }
if (PENDING(w) && REF(w)==0) { if (CAS(&packed, w, w|DEAD)) sbo_free(box); }

/* sweep（墓碑叶，先 release-bump gen） */
for (;;) { w = load(acq); if (DEAD(w)) break;
  if (REF(w) > 0) { if (CAS(&packed, w, w|PENDING)) break; }
  else            { if (CAS(&packed, w, w|DEAD)) { sbo_free(box); } break; } }
```
两条路径对同一个字做 CAS：**只有把 DEAD 从 0 置 1 的那一方执行 `sbo_free`**，因此不会双重释放；
`PENDING` 只是“等最后一个 unpin”的标记。
**unpin（CAS，acq_rel）**：`REF(w)--`；若 `PENDING(w) && REF==0` → CAS 置 `DEAD` 并 `sbo_free`
（最后一个释放者负责回收）。

**ABA 论证**：token 只保存 `(node_id, gen)`；CAS 比较的是**同一个 64-bit 字里的 gen**。
节点 id 被复用时，`art_balloc` 会把 gen 置为更大的全局 epoch，因此复用后的字必然与 token
的 gen 不同 → CAS 失败 → STALE。不存在“gen 未变但 box 已被释放复用”的状态，因为释放
必然先经过 `pending/dead` 且 gen 已 bump；`gen:32` 的回绕需要 2^32 次节点分配，按当前
分配速率不可达（记录为理论边界）。

**顺序**：pin 用 `acq_rel`（读者拿到 pin 后看到的值一定在 pin 之前发布）；sweep 先
`release` 地 bump gen，再 CAS 置 DEAD 并 free；读者的每次取值用 `value_gen` 的
acquire-load / release-store（seqlock）。

**读路径必须同时校验 `recovery_epoch`（屏障超时的兜底）**：token 里保存
`store_epoch = hdr->recovery_epoch`；每次读 body 的 seqlock 从
`{value_gen → value_gen}` 扩为 `{value_gen, recovery_epoch → value_gen, recovery_epoch}`。
屏障超时后 `recovery_epoch` 变化，**即使该进程没 ack，它的下一次读也会判 STALE/重试**，
不会读到已被回收的 box。多一次 load 约 1–2 cycles（Get 21.9 cyc 有余量）。

### 生命周期与内存序

| 事件 | 动作 | 序 |
|---|---|---|
| **ViewResolve / FromRef / ResolveFrom** | 1) 读 `struct_gen`（acquire）2) `refcount++`（acq_rel）3) 再读 `struct_gen`；若变化 → `refcount--` 并返回 STALE | 双重校验：pin 前后 gen 一致才算成功 |
| **读者读 body** | 只做 `value_gen` seqlock（读-读-读），**不加 ref** | 因为 pin 已保证 box 不被释放 |
| **ViewRelease（新增）** | `refcount--`（acq_rel）；若 `pending_free && refcount==0` → `sbo_free` + `struct_gen++` | 最后一个释放者负责物理回收 |
| **ViewDel（前台）** | `has_value=0` + `value_gen++`（release）；**不动 box、不动 refcount** | 墓碑立刻生效；持 token 者仍可安全读旧值（直到 release） |
| **Sweep** | 对墓碑叶：`struct_gen++`（acquire-release）；若 `refcount==0` → 立即 `sbo_free`；否则置 `pending_free=1` | bump gen 在释放之前；释放只发生在 ref=0 |
| **ViewCheck / 读 body** | `store_id` 相等 + `struct_gen` 相等 + `value_gen` seqlock + **`recovery_epoch` 前后各一次** | 顺序一致 |

**无 UAF 的论证**：任何一次对 box 的物理释放都发生在 `refcount==0` 之后（sweep 或最后
一个 releaser）；读者在持有 token 期间 refcount≥1，因此它读到的 box 不会被释放。
`sbo_free` 之后同一 offset 可能被复用，但那时 token 的 struct_gen 已变 → 任何后续 Check
返回 STALE，不会把新 box 当成旧值。

### 孤儿 pin（崩溃恢复场景，**必须解决**）

kvlang 的核心场景是崩溃恢复：持 pin 的进程被 `kill -9` 后 pin 永远不释放 → 无界泄漏。
采用 **「进程注册表 + 恢复屏障」**（可证明，且不需要 per-owner 的节点列表）：

1. **注册表**：shm 头部固定数组 `{pid:u32, starttime:u64, recovery_ack:u64, state:u8}`（N=64 槽）。
   - **认领**：open 时**先扫槽回收死槽**（pid+starttime 判死 → 置 FREE），**再**从 FREE 槽 CAS
     认领（`FREE→MINE`）。顺序必须是“先回收、后判断占满”，否则反复崩溃/重启会把 64 槽耗光，
     出现“全死槽但 open 失败”。
   - **占满策略**：无空槽 → open 返回失败（错误信息明确“too many processes, N=64”），
     **不静默扩容**（扩容需要屏障，留给下一版）。
   - **死槽回收**：sweep 检测 owner 死亡后把槽置 FREE（在恢复屏障完成之后）。
   - **pid 复用**：Linux 读 `/proc/<pid>/stat` 第 22 字段 starttime；两者都匹配才算同一进程。
   - **平台**：本轮只实现 Linux（/proc）。**macOS 不做墓碑回收**：没有可证明的独占锁
     （flock/单实例）时，全量 compaction 不安全，因此 macOS 上 `ViewDel` 仍写逻辑墓碑但
     **sweep 不释放 box**，box 延迟到 `kvspaceClose` 统一释放（语义：墓碑堆积到 close，
     期间空间不回收）。若未来引入 store 级独占锁，再启用 compaction。
2. **检测**：sweep 周期检查每个槽：`/proc/<pid>` 不存在或 starttime 不匹配 → 该 owner 死亡。
3. **恢复屏障**：
   - sweeper 在 hdr 里递增 `recovery_epoch`，把该死进程的槽标为 `recovering`；
   - 每个存活进程在下次任何 `kvspaceView*` 调用时发现 `recovery_epoch` 变化 → 自己所有
     现存 token 一律视为 STALE（进程内把 `store_id` 派生值换掉），并按需重新 Resolve；
   - 存活进程写 `recovery_ack = new_epoch`；
   - 当所有**存活**槽都已 ack（或超时 T），sweeper 把全局 `ref_base` 提升到新代（等价于把
     所有旧 pin 作废），此后 `ref==0` 的墓碑可以安全 free。
   - 死进程不可能再访问 shm（已死），所以作废它的 pin 不会 UAF；存活进程已 ack → 它们的
     旧 token 已 STALE，不会再用旧 body。
4. **超时**：若有存活进程长时间不 ack（挂起），屏障超时后只回收 `pending` 中 gen 更旧的
   箱子，并在审计日志里记 “barrier timeout”（不静默）。
5. 测试：A 持 token → `kill -9 A` → B sweep → 重启 A' → 断言 pin 被回收、B 无 UAF、
   旧 token 失效（这是 ③ 的必测项）。
6. **新增必测**：① 存活进程 `SIGSTOP`（不 ack）→ 屏障超时推进 → 该进程恢复后首次读
   必须 STALE 且无 UAF（靠读路径的 recovery_epoch 校验）；② 注册表占满（第 65 个进程
   open 失败，错误明确）；③ 屏障进行中进程崩溃（sweeper 记录并重试，不卡死）。

## 1.5 退役链（2026-10-25 修订，实现依据；§5 状态表由 §1.5.7 取代）

**问题（已实测确认）**：packed 的 `ref` 是「节点当前 box」的 pin 计数。覆写换 box 或墓碑复活时，
节点指向新 box，旧 box 的 pin 无处记账 → ①旧 box 永不释放（泄漏）；②旧 token 的 unpin
减到新 box 的 ref（计数腐败）；③写者直接 `sbo_free(old)` 会让持 pin 读者 UAF。
`p2_node_at` 的 reset 竞态已修（`d==klen` 判断移出 prefix 块；实测 K=1/4/64 通过），
但复活语义本身仍需按 box 记账。

### 1.5.1 结构

- gens 表每节点 16B → **32B**：`{ packed:u64 | value_gen:u32 | retire_idx:u32 | retire_head:u32 | _rsv:u32 | _rsv2:u64 }`。
  `retire_idx` 指向本节点最近一代退役条目（0 = 无）；`retire_head` 为链头（0 = 空）。
- 退役池：shm hdr 固定数组 `P2_RETIRE_N = 65536` 条 × 32B = **2 MiB**。
  ```
  entry = { off:u64 | node:u32 | gen:u32 | ref:u32 | next:u32 | state:u32 }
  state: FREE | LIVE | RECLAIMING      next: 0 = 链尾
  ```
- 容量依据：长寿命 token 每次跨线程覆写都可能产生一代退役，不能只按 token 上限估。
  取 64 进程 × 1024 代，2 MiB 是 ART 文件的零头；真实上限由**压力测试**给出（池满注入测试）。
- 空闲表：初始化成单链，原子 pop/push（CAS head）。

### 1.5.2 退役发布协议（RETIRING，修掉“快照 ref 与 unpin 竞态”）

packed 的 flags 位新增 **RETIRING = bit30**。**顺序不可交换**：

1. `pool_get()`：先从池取 entry。**池满 → 直接返回 EAGAIN，packed 不动**（不留半退役状态）。
2. CAS packed：仅置 `RETIRING`（gen/ref 不变，acq_rel）。此后 pin 一律失败（守卫 RETIRING）。
3. 读 packed 快照 `ref`。**此值稳定**：RETIRING 期间 unpin 不自减 packed（见 1.5.3），只等待发布。
4. 填 entry `{off, node, gen=旧代, ref=快照}`，`state=LIVE`（relaxed）→ CAS 头插 `retire_head`（release）。
5. release-store `retire_idx = entry`（**发布点**）。等待中的 unpin 在此之后才能路由。
6. CAS packed 重置为 `{gen+1, 0, 0, 0}`（清 RETIRING）；墓碑再置 `box_offset=0`。
7. 池满计数 `pool_full++` + 审计日志；EAGAIN 上抛调用方，runtime 回退字符串 ABI。

**竞态证明**：unpin 在步骤 2 后到达 → 不自减、等 5 发布 → 按 (node, gen) 命中 entry 恰减一次；
在步骤 2 前完成的 unpin → 已反映在步骤 3 的快照里。两种情况下同一 token **不可能**既被快照
计入又重复减 entry。步骤 6 之后到达的 unpin → gen 不符 → 走链，条目已发布可见。
条目找不到只剩“已被 sweep/屏障回收”，no-op 幂等安全。

### 1.5.3 unpin（按代分流 + 有界等待）

- `tok->struct_gen == PK_GEN(packed)` 且无 RETIRING：现行 packed 路径（减 ref；`PEND && ref==0` → DEAD + free）。
- packed 含 RETIRING（且 `tok->struct_gen == PK_GEN(packed)`）：**不自减**，短自旋等
  `retire_idx != 0`（acquire），上界 `P2_RETIRE_SPIN = 1024` 次（含 `sched_yield`，正常微秒内发布）。
  超界 → `ViewRelease` 返回 **EAGAIN**，由 runtime 退避重试（不得无界自旋、不得静默丢弃）；
  `retire_spin_timeout++` 计数 + 审计。writer 已死的情况由 §1.5.10 的恢复路径处理，不靠自旋。
- 否则（gen 不符或等待到发布）：先查 `retire_idx` 指向的条目是否 `node/gen` 匹配，不匹配再沿
  `retire_head` 链扫；命中则 CAS `ref--`；→0 时 CAS `LIVE→RECLAIMING`，
  **赢家** `sbo_free(entry.off)`、摘链、归还池。
- 链上找不到 → 已回收，no-op。

### 1.5.4 sweep

- 墓碑叶（`has_value==0 && box_offset`）：按 1.5.2 退役。
- 回收链上 `state==LIVE && ref==0` 的条目（free + 摘链 + 归还池）。
- 屏障**完成**（所有存活进程 ack、死进程已知）后：把该节点链上所有条目 ref 强制清零并回收；
  依据是代已推进、旧 token 全 STALE，且读路径必须在 body 读前后各校验 `recovery_epoch`（runtime 义务）。
- 屏障**超时**：只回收 `ref==0` 条目，pinned 条目保留待下次，写审计行（不静默）。

### 1.5.5 art_del / deltree

统一为 `has_value=0 + value_gen++` + 退役协议；**删除 ring + 后台 flusher**，
保证 free 只有「退役链最后 unpin / sweep」一个归属。

### 1.5.6 热路径

- Get / 同尺寸 Set-over / ViewDel：**不变**（packed CAS + seqlock，无池操作）。
- 换尺寸 Set / 墓碑复活：一次池分配 + 头插（非 50c 目标路径）。
- pin：不变。unpin：仅 STALE/退役 token 才走链扫（正常 token 走 packed）。

### 1.5.7 状态转移（取代 §5）

| 状态 | has_value | box_offset | packed | 退役链 | 负责者 |
|---|---|---|---|---|---|
| live | 1 | B | {g, ref, 0, 0} | 可有旧代条目 | pin/unpin 正常 |
| tombstone | 0 | B | {g, ref, 0, 0} | — | ViewDel/art_del：value_gen++ |
| retiring | 0/1 | B | {g, ref, RETIRING} | entry 写入中 | 写者：pin 已禁，unpin 等待 |
| retiring | 0 | 0 | {g+1, 0, 0, 0} | entry{g, ref=快照} | sweep/写者退役 |
| retired | 0 | 0 | {g+1, 0, 0, 0} | entry state=RECLAIMING | 最后 unpin 或 sweep free |
| revived | 1 | B' | {g+2, 0, 0, 0} | 旧代条目保留 | shm_set_local 复位 |

### 1.5.8 测试补充（在 §3 之外）

1. 同一 token pin 期间连续覆写 ≥3 次 → 旧 box 全部在最后一次 release 后回收，packed ref 不腐败。
2. del → 重写 → 再 del → 多轮 sweep，链长不增长（条目都回收）。
3. **退役池压力**：注入脚本循环「pin → 覆写」直到池满，断言 EAGAIN + `pool_full` 计数 + 审计行；
   释放 token 后池恢复可用。
4. kill -9 **且链上有条目**：子进程 pin → 父进程覆写退役（entry ref>0）→ kill -9 → 屏障完成 →
   条目强制回收、box free（断言 sweep freed ≥1、池占用归零）。
5. SIGSTOP：屏障超时 → pinned 条目保留；恢复后首次 Check STALE，release 后条目回收。
6. **写者崩溃四点**：注入点 `P2_CRASH_AT=2|4|45|5`（45 = entry 已头插、retire_idx 未发布），
   子进程 `_exit(137)`，新进程 open + sweep，断言：key 可再解析、ASan 无 double free/双重归还、
   池占用归零、`leak_boxes` 只在 2 点 +1；反复 2–3 崩溃 65 次后 open 必须失败并报错。
7. 全套：TSan/ASan、多进程（A sweep / B 读写 / C 只读）、close/重开、refcount 泄漏。

### 1.5.10 写者崩溃恢复（RETIRING 中途 kill -9）

entry 增加 owner 戳 `{owner_pid:u32, owner_start:u64}`（entry 40B，池 65536 × 40B = 2.5 MiB）。
`packed` 的 RETIRING 只由 **owner 已死** 的恢复者清除；owner 存活 → 视为“慢写者”，不做任何修复。

**恢复入口**：`open`（认领槽后）与 `sweep` 的 DFS 遇到 `RETIRING` 节点时执行：

| 崩溃点 | 现场判据 | 恢复动作 |
|---|---|---|
**恢复必须按 owner 戳扫链**，不能只看 `retire_idx`（步骤 4→5 之间崩溃时
`retire_idx==0` 但 entry 已链入）：

| 崩溃点 | 现场判据（扫链，owner 已死 + node/gen 匹配） | 恢复动作 |
|---|---|---|
| 2 后、4 前 | 链上无 entry | **回滚**：CAS 清 RETIRING，gen/ref 原样；旧 token 继续有效 |
| 4 后、5 前 | 有 entry，`box_offset==entry.off` | **回滚**：entry CAS `LIVE→ROLLBACK`（见下） |
| 4 后、5 前 | 有 entry，`box_offset!=entry.off` | **完成退役**：发布 retire_idx + reset `{gen+1,0,0,0}` |
| 5 后、6 前 | 同上两条按 box_offset 判 | 同上；retire_idx 已发布也按同一规则 |
| 6 后 | RETIRING 已清 | 正常：条目由 unpin/sweep 回收 |

**回滚不能立即归还 entry**（`retire_idx` 可能已发布，unpin 正在/已经减它的 ref）：
1. 恢复者 CAS `entry.state: LIVE→ROLLBACK`（acq_rel）。
2. CAS 失败且 state 已是 `RECLAIMING` → 别的赢家已摘链归还，恢复者只清 RETIRING。
3. CAS 成功 → entry 留在链上，**继续接受 unpin 的 `ref--`**；回滚不写 entry 其它字段。
4. unpin：`state ∈ {LIVE, ROLLBACK}` 才减 ref；`ref→0` 时 CAS 到 `RECLAIMING`，
   **赢家**：ROLLBACK 条目只摘链+归还池（**不 free**，box 仍被节点引用）；LIVE 条目才 `sbo_free`。
5. 内存序：state CAS 用 acq_rel；entry 的内存只由终态 CAS 赢家归还池（归还即重写），
   恢复者发布 ROLLBACK 后不再写 entry → 无“池复用后被写坏”的 UAF。
只有 `*→RECLAIMING` 的 CAS 赢家能摘链/归还/free，双重 free 与双重归还都排除。

- **恢复期间**：新 pin 因 RETIRING 失败（STALE）；新写若遇 `RETIRING && owner 存活` → 返回 EAGAIN
  （runtime 回退字符串 ABI），`owner 已死` → 先恢复再写。全部计入审计，不静默。
- **2–3 泄漏有界**：崩溃点 2–3 时已分配未装的新 box 无法通过节点定位 → `leak_boxes` 计数（hdr，
  每次崩溃 +1，审计每笔）。`leak_boxes > P2_LEAK_MAX (64)` 时 **open 失败并明确报错**
  （“kvspace: retire crash leak N>64, refusing open”），不静默累积。可选强化（下一版）：
  open 时用 allocator 元数据扫未引用 box 并回收；本版只保证“有界 + 显式失败”。

**屏障强制回收的判定**：只对 `owner 已死` 且屏障完成（存活槽全 ack）的条目把 ref 强制清零并回收。
**活着但慢**的 owner 条目一律不强制，只回收 `ref==0` 的；避免对可能在读的活进程 UAF。

### 1.5.9 macOS 分支（必须可证）

- 无 `/proc` → 注册表槽永不判死，屏障永不触发；因此**退役链也不 free**（没有“对方已死”的证明）。
- `ViewDel/art_del` 只写墓碑；sweep 为 no-op；**`kvspaceShmClose` 统一释放**本进程能看到的
  全部墓碑 box 与 `ref==0` 的退役条目（语义：延迟到 close，期间空间不回收）。
- 文档与 README 明确：macOS 不提供并发回收语义，不要引用 Linux 的 sweep 保证。

## 1.6 静默屏障（quiescent barrier）可行性评估（2026-10-25 实测，Linux bj-build）

模型：token 不 pin，只存 `(struct_gen, value_gen, box, recovery_epoch)`；同尺寸就地覆写只 bump
value_gen；换尺寸/复活/删除把旧 box 记为垃圾；垃圾超阈值 → 推进 `recovery_epoch`，所有存活进程在
操作边界 ack 后统一 free（超时/卡住 → 放弃本轮并审计）；token 每次使用校验 recovery_epoch。

### 1.6.1 实测数据

| 项 | 值 | 方法 |
|---|---:|---|
| 8B 值的 box 物理容量 | **72B**（body_cap 8） | `p2_barrier_probe boxsizes` |
| 64B 值的 box | 128B | 同上 |
| 同尺寸覆写 8B | **203 ns/op**（≈4.9M op/s） | `ops 300000`，单进程公共 shm API |
| 换尺寸覆写 8↔64 | **239 ns/op** | 同上 |
| 屏障时延（1 存活进程） | 51 µs | `barrier <store> 1 30` |
| 屏障时延（2 / 4 / 8 / 16 进程） | 1.1 / 6.2 / 12.9 / 21.5 ms（worst 0.06/9.7/20.3/39.5 ms） | 同上；1ms 轮询粒度 + 调度 |
| sweep DFS（1 键 / 4096 键） | 5.8 / 39.1 µs | `sweep` |

### 1.6.2 屏障频率与开销（阈值 64 MiB，垃圾 72B/次 → 932,067 次换尺寸）

| 换尺寸占比 | 屏障周期 @4.9M op/s | 开销（8 进程，13ms/次） |
|---:|---:|---:|
| 100% | 0.19 s | **6.8%** |
| 10% | 1.9 s | 0.68% |
| 1% | 19 s | 0.068% |
| 0.1% | 190 s | 0.007% |

**结论**：只要换尺寸/复活写 <10%，屏障开销 <1%；删键也产垃圾（72B/次），按实际删除率另计。
kvlang 栈槽是 8B inline 标量、同尺寸覆写为主，预计占比远低于 10%。**该项仍需真实 workload 计数**
（kvlang 不在 bj-build；需在带计数的后端上跑 tutorial 套件，约 1 小时），但它只是线性影响上表。

### 1.6.3 热路径影响（预测，实现后复测）

- Get / 同尺寸 Set-over / Del 前台：**不变**（就地写 + seqlock）。
- Resolve：**去掉 pin 的 CAS**（≈25 cyc），231/259 只会更低。
- 唯一新增成本：屏障时延（后台）、garbage 记录追加（换尺寸路径，非热路径）。

### 1.6.5 垃圾跟踪（不建逐 box 大表）

- **墓碑（Del/art_del）**：不需要垃圾表。节点保留 `box_offset`，屏障时 ART DFS 直接找到
  `has_value==0 && box_offset` 的叶并 free。
- **覆写/复活（换 box）**：旧 box 已不被节点引用，必须单独记。用 **u64 偏移表**（8B/条，
  大小从 `sbo_allocated_size(off)` 取）：分段数组，`P2_GARBAGE_MAX = 阈值/最小 box`
  （64MiB/72B ≈ 932k 条 ≈ **7.5 MiB**）；写者只 `atomic fetch_add(tail)` 追加，屏障后整表清空。
  表满 → 写者返回 EAGAIN（runtime 回退），不静默丢垃圾。
- 不用“allocator 元数据全扫”：slotsbox 只暴露 `sbo_alloc/sbo_free/sbo_allocated_size`，
  没有枚举接口；加枚举要改 allocator（超出本版）。
- 屏障成本 = 垃圾表 free（O(垃圾条数)）+ ART DFS（O(节点)，实测 1 键 5.8µs / 4096 键 39µs）。

### 1.6.6 发起者与互斥

- 由后端已有的后台维护线程（现 tomb flusher 线程改造）周期性检查 `garbage_bytes >= 阈值`；
  **热写者只做一次 atomic add，不发起屏障**（避免 ms 级尾延迟）。
- 多进程互斥：hdr 里 `barrier_busy` 用 CAS 0→1 选举；只允许一个进程跑屏障，结束后写回 0。
  崩溃遗留的 busy 由屏障入口按 owner 戳（复用注册表槽）判死接管。

### 1.6.7 runtime ack 契约（接入方案必须写）

1. ack 只发生在 **op 边界**（一次 kvspaceView* 调用开始/结束时），读 body 期间绝不 ack。
2. ack 前必须把本进程帧缓存里所有 token 失效（按 `store_epoch` 派生值换掉）。
3. 每次 token 使用前检查 `recovery_epoch`（已是 `ViewCheck` 的现有行为）。
4. macOS：无 `/proc` → 不发起屏障、sweep 不 free、close 统一释放；不提供并发回收语义。

### 1.6.8 屏障时延实测口径（2 vCPU 说明）

bj-build 只有 **2 vCPU**：1 进程 35µs；≥2 进程时 ack 等待被 CFS 调度支配（≈1.5ms，
进程数 2→16 基本不变）。这不是实现瓶颈，而是“可运行进程数 > 核数”。
16 进程 ack <1ms 的目标需在 ≥17 核机器上复测；在 2 核机上应表述为“≤ 一个调度时间片”。

### 1.6.4 风险

1. 存活但长期不 ack 的进程 → 屏障放弃 → 垃圾增长；需审计 + 阈值告警（本轮只审计）。
2. 屏障时延随进程数上升（1ms 轮询放大）；实现时可改短自旋+sched_yield，预计降到亚毫秒级。
3. 无 pin 时“Check 通过后正读 body”的窗口靠 ack 边界保证：ack 只发生在操作边界，读进程在读完
   前不会 ack → 屏障不会 free 它正在读的 box。runtime 必须遵守“ack 只在操作边界”。

## 2. 备选：epoch 公告（若 refcount 成本/生命周期不可行）

- 全局 `epoch`（shm u64）；读者进入保护期时读 `epoch` 并宣告（线程本地 TLS + 每 N 次
  操作发布一次）；sweep 取所有读者公告的最小 epoch，只释放 `tomb_epoch < min_epoch` 的箱。
- 缺点：读者必须周期性发布，停顿的读者会阻塞回收；跨进程需要注册表。
- 结论：token 场景下 refcount 更简单、成本更低（每次 token 创建/释放一次），**选 refcount**。

## 3. 测试计划（未执行，③ 的验收）

1. **同进程 TSan**：读者线程循环 `ViewResolve → Check → 读 body → Release`；sweeper 线程
   循环 `Sweep`；跑 60s，TSan 零报告；断言：读到的值与写入序列一致，无 SIGSEGV。
2. **同进程 ASan**：同上单线程 + 交错，检查 UAF/越界；重点覆盖“Check STALE 后仍读旧 body”
   与“release 后 sweep 立即释放”。
3. **多进程脚本**：A 进程持续 sweep，B 进程 `ViewResolve/Write/Del` 同一子树；C 进程只读；
   断言：B 的 STALE 率与 refresh 成功、C 无崩溃、最终值一致；`kill -9` A 后 B/C 继续可用。
4. **close/重开**：close 时 sweep 线程 join + 队列排空；重开后旧 token 的 `store_id` 不匹配
   → 永久失效（断言 Check 返回 STALE）；新建 token 正常。
5. **refcount 泄漏检查**：所有 token 正常 Release 后，sweep 能回收到 0 个墓碑；崩溃注入后
   记录泄漏量（预期：泄漏，不崩溃）。

## 4. 对已达标数字的影响

- 热路径**不变**：Get 21.9 / Set 43.8 / Del 98.6（p95 131.5）——refcount 只在
  token 创建/释放时发生。
- ResolveFrom 会多一次原子 `refcount++`（≈25 cyc）：231 → 预计 ~256，仍 ≤300；
  需实测确认。

## 5. 叶状态转移表（实现依据）

| 状态 | has_value | box_offset | packed {gen, ref, pend, dead} | 转移与负责者 |
|---|---|---|---|---|
| live | 1 | B | {g, 任意, 0, 0} | 写入（新节点或复活）→ 重置为 {g+1, 0, 0, 0} |
| tombstone | 0 | B | {g, 任意, 0, 0} | `ViewDel`：has_value=0、value_gen++ |
| pending | 0 | 0 | {g+1, >0, 1, 0} | sweep：先 gen++（release），CAS 置 PENDING；不 free |
| dead | 0 | 0 | {g+1, 0, 0, 1} | DEAD 0→1 的唯一赢家 free（sweep 当 ref==0；或最后 unpin 在 PENDING 下） |
| revived | 1 | B' | {g+2, 0, 0, 0} | `shm_set_local` 在 `art_ins` 之后按精确节点重置 packed |

**free 归属**：只有把 DEAD 由 0 置 1 的 CAS 成功者执行 `sbo_free`；PENDING 只是“等最后一个 unpin”。

## 6. 当前实现状态（2026-10-25 11:20）

- **reset 竞态已修**：根因是 `p2_node_at` 的 `d==klen` 判断被错放在 `if (h->prefix_len)` 内，
  键结束在 `prefix_len==0` 的节点时越界返回 -1 → 复活 reset 从未执行。已移出并改为 CAS reset。
  回归：`del_tokens` K=1/4/64 × 多批全过，无 STALE。
- **注册表 + 恢复屏障已实现**：64 槽、`/proc/<pid>/stat` starttime 判活（ENOENT=死）、
  CLAIMING 中间态避免认领竞态、先收死槽再判满、RECOVERING 超时遗留可自愈、
  屏障短自旋后 50µs 轮询；macOS 无 `/proc` 分支不做回收。
  测试：槽满 64/64（第 65 次 open 被拒）PASS ×3；同进程并发 5s：ok=22.5M、bad=0、final=ok。
- **退役链方案已归档**（§1.5），不实现；改用 §1.6 静默屏障（architect 已裁决）。
- **静默屏障可行性数据已出**（§1.6.1–1.6.8）；待补：真实 kvlang 换尺寸占比
  （本机 kvlang 二进制与 v0.3.16 frontend 符号不匹配，需隔离副本重建 runtime）。
- 源码真相在本机 `p2/src/kvspace-c` 与 `p2/src/kvspace-frontend`；bj-build 仅为构建/测量副本。
- 临时探针（未进 patch）：`P2_DEBUG` 打印、`P2_SETSTATS` 写入形态统计、`p2_barrier_probe.c`。
