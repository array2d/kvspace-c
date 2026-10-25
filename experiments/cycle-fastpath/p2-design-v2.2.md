# kvspace-c 50c 快路径：静默屏障文件级设计 v2.2

日期 2026-10-25。分支 `experiment/50cycle-fastpath`（基线 2b64a38，基于 v0.3.16）。

**本文件是实验分支的当前实现口径（source of truth）**：统一垃圾环 + reserve/commit
取代 `HANDOFF.md §8` 第 2 条的“墓碑艺术树 DFS + 8B 偏移/7.5MiB”旧方案（DFS 没有
退役时刻，ack 之后会 free 新 token 指向的 box，反例见 §1.2）。阈值本身不变
（64MiB / 最小 box 72B ≈ 932k 条），条目改为 16B `{off, epoch, meta}`、表 2^21 条
（32MiB VA）。同文件包含：删 pin、Q1/Q2 ack 权威、hdr v5、store_id 修正、
`garb_bytes` 只计 committed、claim 30 位耗尽时 open 硬失败。

依据：`p2-design.md`、`p2-reclaim-model.md`、`p2/results.md`、`bench/p2_barrier_probe.c`，
第 1 步实测（`kvspace-c-50c-step1.md`）：换尺寸写 2.403%（本体最坏 fib 8.10%）、
最小 box 72B、垃圾 1.45 B/写、64MiB 阈值 0.074 次/全量 tutorial。

## v2 相对 v1 的变更（评审回应）

| # | v1 | v2 |
|---|---|---|
| 1 | 维护线程当选后 `ack_self(r)` | **删除**。维护线程永不写任何 ack；leader 的自身进程同样要靠解释器线程 op 边界 ack 或 quiesce 声明（§2.3） |
| 2 | 只有 `kvspaceViewAck` | 新增 quiesce 注册协议 `kvspaceViewQuiesce(h,on)`：阻塞/空闲进程由 runtime **显式声明静默**，维护线程只读该声明，不代签（§2.3） |
| 3 | 快照 `[head,snapshot)` + 8B 偏移表 | 每条 `{off,epoch,claim}` 16B，**变更后**读 epoch 提交；去掉快照；reserve/commit 两段保证 push 失败在变更之前（§1.3-1.5） |
| 4 | 测试 12 项 | 增 A13（leader 当选时读者在 Check→读窗口）、A14（该窗口 SIGSTOP）、A15（quiesce 阻塞进程参与轮次）、A16（从不 ack 的占用者）、A17（push 失败无半写）（§6） |

## v2.1 相对 v2 的变更（评审回应）

| # | 主题 | 补法 |
|---|---|---|
| 1 | reserve 的二选一 | reserve 之后所有出口 = `commit` 或 `cancel`，单出口结构；`tail/garb_bytes` 自洽；close 时断言 `reserved == committed + cancelled`（§1.5，A19） |
| 2 | drain 不跨未决条 | head 只在 EMPTY 或“已 free 的 COMMITTED”上推进；RESERVED / `epoch>=r` 立即停；`garb_bytes` 加减点写死（§2.2，A20） |
| 3 | 弱内存序 | `{has_value,box_offset}` 合并为单个 64-bit `val` 字，读写全走 acquire/release helper；解析侧 mini-seqlock；写进 shm 格式设计与代码（§1.7，A21） |
| 4 | Q2 ABI 语义 | on/off 可见性、重复调用、close/fork、ENOTSUP、四仓 ABI 快照登记；`quiesced` 期间任何 View* 硬错误；runtime-c 阻塞点逐点列出（§5.1/§5.3） |

## v2.2 相对 v2.1 的变更（评审回应）

| # | 主题 | 补法 |
|---|---|---|
| 1 | 状态编码不得占用 `off` | `meta = state:2 \| claim_id:30`；`off` 恒为真实偏移（未提交时不解释）；EMPTY=0、RESERVED/COMMITTED 带 claim；A19/A20 断言改按 `meta`（§1.3/§1.5/§2.2） |
| 2 | claim_id 回绕 | 30 位发号，到 `(1<<30)-1` 时 **open 硬失败并提示重建**，不静默回绕（D14，§1.3） |
| 3 | 发布顺序 | payload（`off/epoch`）先写、`meta` 后 release；读者 acquire 读 `meta` 后只解释 COMMITTED 的 payload（§1.7） |
| 4 | arm64 验收 | A13/A19/A20/A21 列为**发布前置**，x86 通过只算阶段性（§6/§7） |

---

## 0. 裁决摘要

| # | 裁决 | 原因 | §|
|---|---|---|---|
| D1 | **取消 pin/refcount**（等 ack 权威修好后一并批准） | 死进程的 pin 无法安全清（要 per-pin owner = 退役链，明确禁止）；ack/quiesce 静默点提供同等保护 | 4.1 |
| D2 | 不做墓碑 ART DFS，删除时把旧 box 排队 | DFS 无法给出“退役时刻”；环条目自带 epoch | 1.2 |
| D3 | 垃圾表 = **16B 条目环形数组**，cap 2^21（32MiB VA） | 一个阈值周期 932,067 条；多出的 8B 换 per-entry epoch + 不复用的 claim_id | 1.3 |
| D4 | 表满 → **reserve 返回失败，且失败发生在任何键值/节点变更之前**；写路径硬错误 | 丢条目=静默泄漏；失败后旧值必须仍可读、无半写 | 1.5 |
| D5 | 回收判定 = **per-entry epoch < 本轮 epoch**；epoch 在**变更之后**读取提交 | 这一步排序使“提交-变更”窗口两侧的任何 bump 都安全，无需时序假设 | 1.4 |
| D6 | **ack 权威三分**：runtime 作废 token 并 ack；runtime 显式声明 quiesce；维护线程只读、永不代签 | ack 是进程级的，维护线程与解释器同进程 ⇒ 代签会让 leader free 掉解释器正在 Check→读的 box | 2.3 |
| D7 | 增量 ABI：`kvspaceViewAck` + `kvspaceViewQuiesce`；删除 `ViewRelease` | token 是调用方数据；阻塞/空闲进程必须有显式静默注册，不能靠维护线程代签 | 5.1 |
| D8 | 维护线程复用 tomb flusher 位置；ack 等待用共享 futex | 现有结构可复用；futex 消掉 1ms 量化 | 3 |
| D9 | macOS：不并发回收，close 只在最后一个存活进程时清场 | 无 /proc 无死亡证明 | 4.4 |
| D10 | `store_id` 改为每进程单调句柄序号 | 现实现 malloc 复用同一 `kv` 地址会让旧 token 误通过校验 | 2.6 |
| D11 | reserve 之后必须 **commit 或 cancel**，单出口 | 残留条目若带存活 off，drain 之后会 double free | 1.5 |
| D12 | `{has_value,box_offset}` 是**单个 64-bit 原子字**，读写全走 acquire/release helper | 分离读在弱内存上会配错 (gen, box)；box 身份校验兜住失败配对 | 1.7 |
| D13 | `ViewQuiesce` 语义写死且**机械可验证**（quiesced 期间 View* 硬错误） | 静默注册协议不能只靠注释；四仓 ABI 快照同步登记 | 5.1 |
| D14 | `claim_id` 30 位，发号耗尽时 **open 硬失败**，绝不静默回绕 | 回绕会让死者预订被误判为在飞；规范不能留静默边界 | 1.3 |

---

## 1. 垃圾跟踪

### 1.1 三类垃圾，一张表

| 事件 | 旧 box 去向 | 有无垃圾 |
|---|---|---|
| 同尺寸就地覆写（Step 1 的 `same`，361,623 次） | 原地 memcpy，只 bump `value_gen` | **无** |
| 换尺寸（`realloc`，81,861 次）/ 复活时另分配 | 旧 box 入环 | 有 |
| `ViewDel` / `art_del` | 旧 box 入环，`has_value=0`、`box_offset=0` | 有 |
| 节点整棵移除 | 同 `art_del` | 有 |

同尺寸覆写不换 box，没有“旧 box”，不进表；表里的都是从节点上摘下来的 box。

### 1.2 为什么不做墓碑 ART DFS

`p2-reclaim-model §1.6.5` 原计划墓碑靠 DFS 找。新模型下不成立的反例：

1. 进程 P 在第 r 轮 ack 之后继续跑；
2. P 解析活键 K 得到 token T，body 指向 box B；
3. 另一进程删除 K → `has_value=0`，B 成为墓碑；
4. leader 的 DFS 在 ack 之后才扫到该叶，看到墓碑就 free B；
5. P 的 T 是 **ack 之后**创建的，没被本轮 ack 作废 → UAF。

根因：DFS 只看到“现在是墓碑”，看不到退役发生在轮次边界哪一侧。补法要在节点上
存退役 epoch（改 ART 布局或加宽 gens），而**入环时记录 epoch** 天然有这一信息，
且成本从 `O(节点数)` 降为 `O(垃圾条数)`。

### 1.3 环表格式与容量

独立映射 `<path>.garb`（同 `<path>.gen`：`open+ftruncate+mmap(MAP_SHARED)`，
`MAP_NORESERVE` 预留）：

```c
/* meta = state:2 | claim_id:30。off 永远是真实 box 偏移（未提交时无意义），
   绝不用 off 的哨兵值编码状态——sbo 的 offset 0 可以是合法 box。 */
typedef struct {
    uint64_t off;      /* COMMITTED 时的真实 box 偏移；其它状态不解释 */
    uint32_t epoch;    /* COMMITTED 时 commit 读到的 recovery_epoch；其它状态 0 */
    uint32_t meta;     /* 发布字：state=b0..1（0 EMPTY / 1 RESERVED / 2 COMMITTED），
                          claim_id=b2..31（open 时领取，>=1；0 只在 EMPTY 出现） */
} garb_ent_t;          /* 16B */

enum { GE_EMPTY = 0, GE_RESERVED = 1, GE_COMMITTED = 2 };
#define GE_STATE(m)   ((m) & 3u)
#define GE_CLAIM(m)   ((m) >> 2)
#define GE_MAKE(st, id) ((((uint32_t)(id)) << 2) | (uint32_t)(st))
```

**发布顺序**：写者先写 payload（`off`、`epoch`），再 release 写 `meta`；
读者先 acquire 读 `meta`，state 为 COMMITTED 时 payload 已被 release 覆盖。
RESERVED 时 `meta` 同时带着 `claim_id`，`off` 不解释。

`claim_id` 不用注册槽下标：槽只有 64 个、被反复回收（tutorial 一次 679 个进程实例），
一个死在 reserve→commit 窗口的进程留下的旧槽号会被后来的进程重新占用，
“槽活着”会把一条永远等不到 commit 的条目永久卡在 head 上。`claim_id` 在每次 open 时从
hdr 的 `claim_seq` 原子领取，槽回收后新 claim 必然不同；leader 用
“64 个存活槽里是否存在该 claim_id”判活。

**claim_id 不回绕**（D14）：`claim_id` 30 位、从 1 开始发号；open 时若
`claim_seq == (1u<<30) - 1` 则 **open 直接失败**：

```
kvspace: claim_id exhausted (2^30 opens): rebuild the store
```

不静默回绕（回绕会让死者预订被误判为在飞，甚至更糟）。1.07×10⁹ 次 open
（tutorial 一次 679 个实例）用不到；真到上限就重建 store，符合 p5。

```
cap  = 1 << 21 = 2,097,152 条 = 32 MiB VA
head / tail : hdr 内绝对单调下标
```

容量换算：

```
64 MiB / 72 B（实测最小 box）          = 932,067 条 = 14.9 MiB（表下限）
16 进程 × 4.9M op/s × 8.1% × 0.05s    ≈ 317k 条（一轮并发追加）
932k + 317k = 1.25M < 2.10M           ✓
```

阈值映射：写者 commit 时 `garb_bytes += box_cap`；维护线程每 50ms 检查
`garb_bytes >= 64 MiB` 才尝试选举。回收后按实际释放容量减回去。
无存活对端时（open 时其它槽全死 / close 时自己是最后一个）允许一次机会性屏障。

### 1.4 可回收判定：per-entry epoch（正确性核心）

**规则**：条目在 `commit` 时读当前 `recovery_epoch` 写入 `epoch` 字段；
第 r 轮 leader 只释放 `GE_STATE(meta)==GE_COMMITTED && epoch < r` 的条目。

**写入顺序**（这是 v2 的关键）：

```
reserve()  → 变更节点（retire）  → commit(epoch = 读当前 recovery_epoch)
```

顺序不能反。反例（v1 的快照规则隐含的顺序）：先 commit 再变更时，若 bump 插在中间，
条目 epoch=e < r，而退役发生在 bump 之后，bump 之后创建的 token 不在本轮 ack 覆盖内。

**证明**（box B 的条目 epoch = E，leader 在 r > E 轮释放）：

1. commit 读到的当前 epoch 是 E ⇒ 退役（节点不再指向 B）发生在 E 读之前，
   因此发生在其后任何一次 bump（包括到 r 的那次）之前。
2. 引用 B 的任何 token 都只能在退役之前创建（退役后节点不指向 B，`has_value=0`，
   三条 Resolve 路径都不可能返回 B）。
3. ⇒ 该 token 创建于 bump r 之前；第 r 轮 ack 全部到齐意味着每个存活进程在 ack 之前
   作废了所有此类 token（§2.3），而死进程不再访问 shm。
4. ⇒ 释放 B 时没有任何存活 token 指向它。

`epoch` 是 u32，比较用模运算 `(int32_t)(r - epoch) > 0`（200ms 一轮，回绕 27 年）。

### 1.5 reserve / commit 与表满（D4）

```c
int  garb_reserve(kvspace_t *kv, uint64_t *idx);            /* 失败在变更之前 */
void garb_commit (kvspace_t *kv, uint64_t idx, uint64_t off);/* 变更之后调用 */
void garb_cancel (kvspace_t *kv, uint64_t idx);
```

**单写者降级（v2.3）**：契约为“一个 store 同一时刻只有一个写者进程”
（kvspace-c #58），因此 `entry/tail` 只有写者写，**不需要任何 CAS**：

- `reserve`：acquire 读 `head/tail`，检查 `tail - head < cap`；写
  `meta=GE_MAKE(GE_RESERVED, claim)`（release），再 release 写 `tail = t+1`。
  槽位若不是 EMPTY：只可能是上一个写者死在 store 与 tail 之间（未发布、`i==tail`），
  此时查 claim 存活，死者 → release 写 EMPTY 后重试；**活 claim 占着 tail 槽 =
  契约被违反 → 返回 -1 硬错误**（不猜、不等待）。满 → 返回 -1。
- `commit`：写 `off=retired_off`、`epoch=load(recovery_epoch, acquire)`，最后 release 写
  `meta=GE_MAKE(GE_COMMITTED, claim)`；`committed_bytes += cap`（写者单方，普通加；
  leader 只 acquire 读）。
- `garb_bytes` 拆成两个计数：`committed_bytes`（只写者改）与 `freed_bytes`（只
  leader 改），阈值/满检查读 `committed - freed`。写者与 leader 可能是不同进程，
  两个计数各自单写者，所以都不需要 RMW。
- `cancel`：release 写 `meta=GE_EMPTY`（claim 位清 0）。reserve 之后、变更之前任何失败
  都必须 cancel。
- 读者一律先 acquire 读 `meta`；只有 COMMITTED 才解释 `off/epoch`（release 覆盖它们）。

**不变量（D11）**：`reserve` 与 `commit` 之间只有“变更节点”一步；所有出口
（正常、`kv_sbo_alloc` 失败、`art_ins` 失败、`has_value` CAS 失败）= `commit` 或
`cancel` **二选一**，没有第三条路。实现用单出口结构（`goto out` + `bool settled`），
禁止在 reserve 与 commit 之间 `return`。`tail` 单调，`cancel` 不回退 tail（条目由
drain 当 EMPTY 跳过）；`committed_bytes - freed_bytes` 只统计已提交、未释放的物理
容量（RESERVED/EMPTY 不计）。
每进程 close 时断言 `reserved == committed + cancelled`（P2_DEBUG 下 abort）。

三条路径的落点（**都不产生半写**）：

| 路径 | reserve 之后 → commit 之前 | 失败时 |
|---|---|---|
| `shm_set_local` 换尺寸 | alloc 新 box → memcpy 新 box → `art_ins` → 写 `box_offset`（变更） → commit | alloc/art_ins 失败 → cancel + 释放新 box（从未被引用），旧值原样可读 |
| `ViewDel` | `art_val_cas({1,off}→{0,0})`（一次原子字完成退役） → `value_gen += 2` → commit | CAS 失败 → cancel，返回 STALE |
| `art_del` | 同上 | 同上 |

表满时 `reserve` 返回 -1：此时**尚未动过节点、未动过值**，Set 失败后旧值仍可读、
无新 box 泄漏。错误精确上抛（`shm_set_local` / `ViewDel` 返回真实失败），
每进程打印一次：

```
kvspace: garbage ring full (cap=2097152, head=…, tail=…, bytes=…):
head entry #… state=… claim=… epoch=… is holding the drain (RESERVED / epoch >= round)
```

不选另两条：**写者提前发起屏障**违反尾延迟约束（ms 级）；
**EAGAIN→回退字符串 ABI** 的这次写照样换 box、照样产生同样垃圾，只是包庇了一层（p0）。
**静默丢弃**等于永久泄漏 box（p1），排除。

**运维路径**：`kvspaceViewSweep(h)` = 同步强制跑一轮，用于卡死进程被处理
（kill/SIGCONT）之后立刻清空环；有别的 leader 在跑时返回“忙”。

### 1.6 写入点清单（实验分支代码位）

| 位置 | 现状 | v2 |
|---|---|---|
| `shm_set_local` 换尺寸分支（kvspace.c:2562-2565） | `kv_sbo_free(old->box_offset)` | reserve → alloc/memcpy/art_ins/box_offset → commit |
| `ViewDel`（kvspace.c:705-721） | 只置墓碑，box 留给 DFS | reserve → `art_val_cas({1,off} → {0,0})` → `value_gen += 2` → commit |
| `art_del`（kvspace.c:1690-1698） | `kv_sbo_free_deferred` | 同上 |
| `kv_sbo_free_deferred` / `p2_flush_tomb` / `p2_flusher` / `tomb_ring` | 每进程 256 条延期环 + 立刻 free | **整段删除** |
| `p2_sweep_node` / `ViewSweep` | DFS 清墓碑 | DFS 删除；`ViewSweep` = 同步一轮屏障 |

### 1.7 节点变更的原子性与内存序（D12）

**布局**：现在 `art_hdr_t` 里 `has_value:1` 与 `box_offset:63` 是位域，跨进程并发
读写下无法证明不撕裂。v5 把两者合并成一个天然 8 字节对齐的 64-bit 字，节点总大小不变：

```c
typedef struct {
    uint8_t  type, prefix[ART_PREFIX_MAX], prefix_len;  /* 0..12  */
    uint16_t count;                                     /* 12..14 */
    uint16_t _pad;                                      /* 14..16 */
    uint64_t val;                                       /* 16..24，bit0=has_value，bit1..63=box_offset */
} art_hdr_t;                                            /* 24B，与现布局同尺寸 */
```

**节点地址必须 8 字节对齐**（v2.2 补）：`val` 天然 8 对齐是上面的前提，但
blockmalloc 的 `blocks_init` 按容量自动选 2/4/8 字节块头，256MB slab + 2112B 块
选到 **4 字节头**：`blockdata_offset = id*(4+2112)+4`，节点地址在 `8k`/`8k+4`
交替。于是半数节点的 `val` 非对齐，其中跨 64B 行的那个（每 16 个节点 1 个）让
`lock cmpxchg` 退化为 **split lock**（本 pod 实测 ~7k tick/次，del_tokens
p95 25.9k）；非对齐 8B 原子在 x86 上还可能撕裂，读者与写者并发时读到
`has_value`/`box_offset` 不一致的组合——这是正确性问题，不只是性能。
因此：创建 store 时在 `blocks_init` 之后强制 `sizeof_block_head = 8`
（stride = 8+2112 = 2120 ≡ 0 mod 8，data = id*2120+8 恒 8 对齐），并断言
`(sizeof_block_head + block_size) % 8 == 0`；open 时校验头宽 = 8，否则
"unsupported store format; rebuild"。

**所有访问只经三个 helper**，仓库里不再出现裸位域读写（这条连同上面的布局写进
kvspace-c 的 shm 格式设计文档，并由 A21 的 grep 检查守住）：

```c
static inline uint64_t art_val_load(const art_hdr_t *h) {
    return __atomic_load_n(&h->val, __ATOMIC_ACQUIRE);
}
static inline void art_val_store(art_hdr_t *h, uint64_t v) {
    __atomic_store_n(&h->val, v, __ATOMIC_RELEASE);
}
static inline bool art_val_cas(art_hdr_t *h, uint64_t *exp, uint64_t nv) {
    return __atomic_compare_exchange_n(&h->val, exp, nv, false,
                                       __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
```

| 事件 | 原语 | 序 |
|---|---|---|
| 解析读 `has_value+box_offset` | `art_val_load` | acquire |
| realloc 换 box（变更） | `art_val_store(ArtVal(1, new_off))` | release |
| delete / art_del（变更） | `art_val_cas(ArtVal(1, off) → ArtVal(0, 0))` | acq_rel |
| `gens[].struct_gen/value_gen` 读 / bump | atomic load / add | acquire / release |
| commit 读 `recovery_epoch` | atomic load | acquire |
| commit 写 payload（`off/epoch`）后 release 写 `meta` | atomic store | release |
| Q1 `recovery_ack` / Q2 `quiesced` 写 | atomic store | release |
| leader 读 `ack / quiesced / entry.meta` | atomic load | acquire |

**解析侧 mini-seqlock**（弱内存上避免 (gen, box) 配错）：

```
retry ≤ 2:
  g1 = load(gens[id].struct_gen, acquire);
  v  = art_val_load(h);                    // 一次拿到自洽的 (has_value, box)
  g2 = load(gens[id].struct_gen, acquire);
  if (g1 == g2) 用 (g1, v) 建 token; 否则重试
```

即使重试后仍不匹配，Check 的 box 身份校验（`tok->head == sbo_data + box_offset`）
也会把它判 STALE，不会放出读写错 box 的 token——这条校验在弱内存下不是冗余。

**证明依赖的四条 happens-before 边**（写进 shm 格式设计文档与代码注释）：

1. 读者最后一次读 body →（program order）→ 读者的 ack/quiesce release store；
2. leader 的 acquire load 看到该 store ⇒ 读完成 happens-before free；
3. 写者的 retire release store →（program order）→ commit 的 epoch acquire load
   → commit 的 `off` release store；
4. leader acquire 读 `off` 后看到 `epoch < r` ⇒ retire happens-before free。

---

## 2. 屏障协议

### 2.1 token 与校验点

```c
typedef struct {
    uint8_t *head, *body;
    uint32_t headlen, value_len;
    uint64_t cap, body_cap;
    uint32_t struct_gen, value_gen;
    uint64_t store_id;
    uint32_t node_id;
    uint32_t _pad;        /* 解析时的 recovery_epoch */
} kvspaceViewToken_t;     /* 与 patches2 同形；语义为“无 pin” */
```

`kvspaceViewCheck`（0=有效，1=STALE）依次校验：

1. `tok->store_id == kv->store_id`；
2. `tok->_pad == hdr->recovery_epoch`（acquire）；
3. `gens[node_id].struct_gen == tok->struct_gen`；
4. `v = art_val_load(node)`（一次 acquire，取到自洽的 has_value+box_offset）：
   `has_value == 1`（**新增**：删除后 cached token 必须立刻 STALE），且
   `tok->head == kv->sbo_data + box_offset(v)`（box 身份兜底，弱内存下不冗余）。

检查点：Resolve/FromRef/ResolveFrom 填字段；Refresh 重解析；每次用 body 前 Check；
`ViewWrite` 写前 Check + box 身份 + `len<=body_cap`；`ViewDel` 删除前 Check；
runtime 静默点读 epoch（§5.3）。

`value_gen` 只服务 >8B body 的 seqlock；A 路径只提升 `FIXED_SMALL`（≤8B，8B 对齐），
单条原子 load，不需要 seqlock。

### 2.2 一轮的时序

```
维护线程（任意进程）：
  if (garb_bytes < 64MiB) return;
  if (!elect()) return;                       // 活着的 owner 在位：跳过，不等待
  r = fetch_add(&recovery_epoch, 1, acq_rel) + 1;
  mask = 存活槽掩码（/proc 只扫一遍）;
  ok = wait_round(r, mask, P2_BARRIER_MS=200);
  if (!ok) { audit_abort(r, mask); release_busy(); return; }
  drain(r);                                    // 只释放 epoch < r 的 COMMITTED 条目
  release_busy(); round_ok++;
```

`wait_round` 的满足条件（每个存活槽）：

```
slot->recovery_ack == r          // 解释器线程在 op 边界作的 ack（§2.3 Q1）
|| slot->quiesced                // runtime 显式声明“本进程无任何 token，直到退出声明”（Q2）
```

`drain(r)`：

```
for (i = head; ; i++) {
    ent = load(entry i % cap, acquire);          // meta 是发布字
    switch (GE_STATE(ent.meta)) {
    case GE_EMPTY:     advance(i); continue;
    case GE_RESERVED:  if (!claim_alive(kv, GE_CLAIM(ent.meta))) {
                           store(ent.meta, GE_EMPTY); leak_boxes++; audit;
                           advance(i); continue;
                       }
                       return;                   // 活着的预留者在飞（含 SIGSTOP）：本轮到此为止
    case GE_COMMITTED: if (!((int32_t)(r - ent.epoch) > 0)) return;
                       uint64_t cap = sbo_allocated_size(ent.off); // 必须在 free 之前
                       store(hdr->garb_freeing, ent.off);   // 崩溃标记
                       store(ent.meta, GE_EMPTY);           // 先消费，再 free
                       advance(i);
                       kv_sbo_free(ent.off);
                       fetch_sub(garb_bytes, cap);          // 用 free 前的容量
                       continue;
    }
}
```

- **head 推进规则（D11）**：head 只在两种情况下推进——(1) `meta.state==EMPTY`
  （已消费或已取消）；(2) `meta.state==COMMITTED` 且 `epoch < r`，**且本步完成了 free**。
  遇到 RESERVED（活 claim）或 `epoch >= r` 立刻停止本轮；**绝不跳过未决条**——跳过
  等于该 box 永不释放，且它的槽位在 `tail-head` 回落后会被新预留覆盖，造成条目丢失。
- head 每推进一条就写回 hdr（崩溃后下一个 leader 从精确位置继续）。
- **先消费再 free**：leader 崩在两者之间只漏一个 box，绝不 double free。
- **`garb_bytes` 加减点**（唯一口径）：commit 时 `+= sbo_allocated_size(off)`；
  drain 释放时 **先取 `cap = sbo_allocated_size(off)`、free 之后再 `-= cap`**
  （free 后分配器元数据可能变，顺序反了会记错）；reserve / cancel / EMPTY 推进 **不变**。
  因此 `garb_bytes` 恒等于“已提交但未回收的物理垃圾字节”，不含在飞预留。
- 不要求一轮清空；`RESERVED` 或 `epoch >= r` 处停止，其余留给下一轮。
- 一轮里若有条目被“停止点”挡住，本轮仍算成功（已释放的部分有效）。

### 2.3 ack 权威（D6，v1 矛盾在此修正）

**事实**：ack 是**进程级**的（注册槽按 open 计），而解释器线程与维护线程同进程。
维护线程 `ack_self` 会让 leader 在解释器仍处于 **Check→读 body** 窗口时 free
该 box → UAF。v1 的 §2.2 与 D6 自相矛盾，v2 删除一切代签。

**三层权威**：

| 层 | 谁执行 | 含义 | 后端动作 |
|---|---|---|---|
| Q1 op 边界 ack | runtime 的解释器线程 | “我在两条指令之间，已作废全部 view token” | 写 `slot->recovery_ack = ep`（release） |
| Q2 静默声明 | runtime 的任意线程（进入阻塞/空闲前） | “本进程现在没有任何 view token/指针，且直到退出声明前不会再创建” | 写 `slot->quiesced = 1`（release）；退出写 0 |
| 读取 | **维护线程只读** | 维护/选举/等待时读上面两个字 | 永不写 ack/quiesced |

两条声明的内存序论证相同：

- Q1：解释器最后一次读 body **sequenced-before** 作废（本地）**sequenced-before**
  `ViewAck` 的 release store；leader 的 acquire load 看到它 ⇒ 读完成 happens-before free。
- Q2：最后一次读 body → 作废 → `Quiesce(1)` 的 release store；leader acquire 读到
  `quiesced=1` ⇒ 读完成 happens-before free。**声明之后**进程不再创建 token，
  直到 `Quiesce(0)`；`Quiesce(0)` 之后创建的 token 都晚于本轮 bump，
  按 §1.4 不在本轮释放集内。因此 `quiesced` 不需要匹配 epoch。

**两步 ack（Q1）**：

```c
int kvspaceViewAck(void *h, uint32_t *epoch_inout);
/* *epoch_inout == 当前 epoch：写 slot->recovery_ack = *epoch_inout（release），
   ack_seq++（release）+ futex_wake，返回 0；
   否则：*epoch_inout = 当前 epoch，返回 1（= 先作废 token 再来）。 */
```

```c
uint32_t ep = kv->view_epoch;
for (;;) {
    int rc = kvspaceViewAck(h, &ep);
    if (rc != 1) { kv->view_epoch = ep; return; }
    kvlangBodyInvalidateViews(kv);        /* 只作废读 view，不释放内存 */
    kv->view_epoch = ep;
}
```

**维护线程的自身进程怎么算**：

- 解释器在跑 → 每 64 条指令 Q1 ack，正常。
- 解释器阻塞在 watch/sleep → runtime 在进入阻塞前已 `Quiesce(1)`（§5.3），
  维护线程看到 `quiesced` 即满足，不代签。
- 解释器陷在**没有声明静默**的长 native op 里 → 本轮超时放弃（审计）；
  **不允许**维护线程替它签。
- 纯持有进程（open 了但从不执行 kvlang op）：runtime 在 open 后立即
  `Quiesce(1)` 并保持；这是显式注册，不是代签。

**leader 等 ack 的实现**：共享 futex `hdr->ack_seq`。ack 的写与 `ack_seq++` 都 release；
`Quiesce(1)` 在写 `quiesced` 之后也 `ack_seq++` 并 wake（让 leader 立刻重扫）。
`futex_wait(&ack_seq, seq, min(5ms, 剩余))`，总截止 200ms。

**本轮开始时已死的槽**不需要 ack；`mask` 在 bump 之后算。中途死亡的进程不会被
`mask` 重新评估，最多浪费一次超时，下一轮（50ms 后）重算。

### 2.4 超时 / 卡死 / 死亡

- 超时（默认 200ms）：放弃本轮，一条都不 free，`round_aborted++`，stderr 审计
  （epoch、未满足的槽：pid/starttime/ack/quiesced）。
- 卡住的存活进程（SIGSTOP、长 op）：永远放弃，**绝不强制 free**。垃圾涨到环满时
  写路径硬错误（§1.5）；卡死进程被 kill/SIGCONT 处理后，运维调 `ViewSweep` 清空。
- 死亡判定：pid + `/proc/<pid>/stat` 第 22 字段（现有 `p2_proc_starttime` /
  `p2_slot_alive`，kvspace.c:374-421）：ENOENT 死亡；starttime 不同 = pid 复用；
  读不到 → **保守视为存活**。
- 新进程中途 open：claim 时 `recovery_ack = 当前 epoch`（kvspace.c:474-477），
  此刻它没有 token，等价于立即 ack。

### 2.5 选举 / 接管

`barrier_state` 是**单字 CAS 选举字**，编码与环条目相同：`state:2 | claim_id:30`
（FREE=0 / OWNED=1）。owner 身份就是 claim_id，不需要额外戳，也没有 CLAIMING
过渡态——避免“写戳被后来者覆盖”和“死在过渡态”两种卡死：

```
elect():
  cur = load(barrier_state)
  cur == FREE                     → CAS(FREE → OWNED|mine) 成功即当选
  cur == OWNED && GE_CLAIM(cur) == mine       → false（本进程已有 leader 线程）
  cur == OWNED && claim_alive(GE_CLAIM(cur))  → false（活 owner：跳过，不等待）
  cur == OWNED && !claim_alive(...)            → CAS(OWNED|old → OWNED|mine)（死者接管）
  其它                            → false（非法状态，不猜）
```

`claim_alive` = 扫 64 槽：存在 `state==MINE && claim_id==claim` 且 pid+starttime 存活。
`barrier_pid/barrier_starttime` 只作诊断字段（当选时写入），不参与裁决。

### 2.6 store_id 与崩溃审计

`store_id = ((pid)<<32) ^ (uintptr_t)kv`（kvspace.c:2043）在 close 后重开、
malloc 复用同一地址时会让旧 token 误通过校验。改为每进程单调序号：

```c
static uint32_t g_store_seq;
kv->store_id = ((uint64_t)getpid() << 32) | ++g_store_seq;
```

token 只在创建它的进程内消费，进程内唯一即可。

审计字：`round_ok / round_aborted / leak_boxes / garb_freeing`。
`garb_freeing != 0` ⇒ 上一个 leader 崩在消费与 free 之间：`leak_boxes++`、审计、清 0。
每次崩溃最多漏 1 个 box。`RESERVED` 条目的 claim 在所有存活槽里都找不到：
取消预订、`leak_boxes++`、审计（可能漏一个已退役但未 commit 的 box）。

---

## 3. 后台线程与选举

### 3.1 线程生命周期

- **懒启动**：open **不创建**线程；`garb_bytes >= 64MiB` 时才创建（commit 后检查一次 +
  open 时检查一次）。低于阈值的 workload（benchmark 全部 case、tutorial）不会为每个进程
  付 `pthread_create+join` 的 ~43µs（实测：44.35 → 87.54 µs/open）。创建失败 → 该次写/
  open 硬错误（不静默降级）。
- `kvspaceShmClose`：若已启动，`stop=1` → 唤醒 → join；Linux 下 close **不 free**
  垃圾；若自己是最后一个存活进程，先跑一轮机会性屏障。
- `fail:` 路径同样 join（未启动则跳过）。

### 3.2 维护线程主循环

```
while (!stop) {
    wait_private_futex(&maint_seq, 50ms);       /* 进程内私有字，定时醒 */
    if (stop) break;
    if (garb_bytes < THRESH) continue;
    if (!elect()) continue;
    run_round();
}
```

维护线程**只读** `recovery_epoch / slots[].recovery_ack / slots[].quiesced`，
**只写**选举字、`garb_*`、审计字。它不 ack 任何槽（包括自己）。

### 3.3 futex 取代轮询

| 位置 | 现状 | v2 |
|---|---|---|
| leader 等 ack | `sched_yield`×4096 后退 `usleep(50)`（kvspace.c:770-777） | 共享 futex `hdr->ack_seq`，5ms 分片 / 200ms 截止 |
| 维护线程定时 | `pthread_cond_timedwait`（tomb flusher，kvspace.c:1097-1105） | 私有 futex 定时等待（50ms） |
| 写者通知 | tomb ring 满时 `cond_signal` | **删除**；写者只做原子操作，无 syscall |

`FUTEX_WAIT` 返回 EAGAIN 只表示 seq 变了，直接重扫。

### 3.4 fork / 多句柄

- 句柄不 fork 安全：`pthread_atfork` child 钩子置 `forked_child=1`，此后任何
  `kvspaceView*` 返回 -1（硬失败），调用方必须重新 open。runtime-rs 用 `exec` 派生，不受影响。
- 同进程可 open 多个句柄（各占一个槽，≤64）：每句柄各自 ack/声明 quiesce，
  屏障等所有槽，不会漏。

---

## 4. 与现有实现的关系

### 4.1 删除 pin / refcount（D1）

删除 `P2_PEND/P2_DEAD/PK_REF/PK_MK`、`p2_pin`、`p2_unpin`、ViewRelease 的 unpin、
sweep 里的 PEND/DEAD CAS。`kv_gens_t` 回到 `{uint32_t struct_gen; uint32_t value_gen;}`
（8B/节点，不变）。`struct_gen` 改 `atomic_add(1)`；id 复用仍置全局 `gen_epoch`。
`value_gen` 所有静止态偶数写改成 +2（现 delete/revive 是 +1，破坏 seqlock 奇偶）。

不保留 pin 的理由：pin 的正确回收需要 per-pin owner（packed 字只有计数），
即退役链（禁止）；ack/quiesce 已给同等保证，且省掉 Resolve 的 25 cyc CAS。

### 4.2 删除 tomb ring / flusher

`kv_sbo_free_deferred` / `p2_flush_tomb` / `p2_flusher` / `tomb_ring` / `tomb_mu` /
`tomb_cv` 全删。保留 `kv_sbo_free`（只由 leader 在 drain 里调用）。

### 4.3 hdr v5（format_version = 4）

```c
typedef struct {
    char magic[12];
    uint64_t art_slab_size, sbo_head_size, sbo_data_size;
    int32_t art_root;
    uint64_t gens_size, gen_epoch;
    uint32_t format_version;      /* = 4（v2.3：对齐 + 单写者降级） */
    uint32_t recovery_epoch;      /* 屏障轮次（模比较） */
    uint32_t ack_seq;             /* 跨进程 futex 字 */
    uint32_t barrier_state;       /* state:2 | claim_id:30（FREE=0/OWNED=1） */
    uint64_t barrier_pid, barrier_starttime;
    uint64_t garb_cap, garb_head, garb_tail;
    uint64_t committed_bytes;     /* 只写者改 */
    uint64_t freed_bytes;         /* 只 leader 改 */
    uint64_t garb_freeing;
    uint64_t round_ok, round_aborted, leak_boxes;
    uint32_t claim_seq;           /* 30 位发号；到 (1<<30)-1 时 open 硬失败 */
    uint32_t _pad;
    p2_slot_t slots[64];          /* 新增 quiesced / claim_id */
} kvspace_hdr_t;

typedef struct {
    uint32_t pid, state;
    uint64_t starttime;
    uint64_t recovery_ack;
    uint32_t quiesced;            /* Q2 声明，release/acquire；claim 时置 1 */
    uint32_t claim_id;            /* open 时领取（1..(1<<30)-1），reserve 条目引用 */
} p2_slot_t;
```

`art_hdr_t` 同步改成 §1.7 的 v5 布局（`has_value+box_offset` 合并为单个 `val` 字，
节点仍是 24B，ART 内存不增长）。

hdr 变长 ⇒ `ART_OFF` 变、新增 `<path>.garb`、节点首部布局变 ⇒ **格式不兼容**：
`format_version != 4`（或 ART 块头宽度不是 8）的 shm store 在 open 时明确报错并提示
重建（不迁移、不兼容，p5）。3→4 的差别只有 `garb_bytes` 拆成
`committed_bytes/freed_bytes`（单写者降级）。

### 4.4 macOS

`#if !defined(__linux__)`：无 /proc ⇒ 槽永不判死 ⇒ 不选举、不推进 epoch、不 drain；
删除只写墓碑并入环；`kvspaceShmClose` 仅当注册表里无其它存活槽时清空环；
文档明确“macOS 不提供并发回收语义”。

---

## 5. runtime 接入

### 5.1 ABI 增量（D7）

| 符号 | 状态 | 返回 |
|---|---|---|
| `kvspaceViewResolve / FromRef / ResolveFrom / ResolvePrefix / Check / Write / Del / Refresh` | patches2 已定义，签名不变 | 0 / 1 STALE / -1 / -2 ENOTSUP |
| `kvspaceViewAck(void *h, uint32_t *epoch)` | **新增**（Q1） | 0 / 1（先作废再来）/ -1 / -2 |
| `kvspaceViewQuiesce(void *h, int on)` | **新增**（Q2） | 0 / -1 / -2 |
| `kvspaceViewRelease` | **删除** | — |
| `kvspaceViewSweep(void *h)` | 语义改为“同步强制一轮”，返回释放条数 | ≥0 / -1（忙）/ -2 |
| durable/fs/redis | 全部 View\* + Ack + Quiesce 返回 -2 | 调用方回退 |

前端沿用 `kvspaceSlotView` 的**可选 dlsym**：缺符号 → -2，runtime 永久回退字符串 ABI。

**`kvspaceViewQuiesce(h, on)` 语义（D13）**：

| 项 | 规定 |
|---|---|
| 初始值 | **open/claim 时 `quiesced=1`**（新进程还没有任何 token，天然静默）；runtime 开始使用 view 时必须显式 `ViewQuiesce(h,0)`，纯持有/空转进程保持 1，不会阻塞轮次 |
| 参数 | `on` 只接受 0/1，其它 → -1（不猜） |
| on=1 | 前置：调用线程已作废本进程**全部** view token（runtime 责任）。后端 `slot->quiesced=1`（release）→ `ack_seq++`（release）→ `futex_wake` → 0 |
| on=0 | `slot->quiesced=0`（release）→ 0；此后 runtime 必须重新解析，旧 token 一律无效。runtime 启动（首次使用 view 前）必须调一次 |
| 幂等 | 重复 on=1 / on=0 均返回 0；on=1 时不得有 in-flight token 使用 |
| 可见性 | leader 用 acquire 读 `quiesced`；release/acquire 配对保证“最后一次 body 读”先于 free（§1.7 边 1-2） |
| 机械校验 | backend 维护进程内镜像 `kv->quiesced`；`quiesced` 期间任何 View\*（除 Ack/Quiesce）返回 **-1**，不静默回退 |
| close | 随注册槽释放，无需先 on=0；close 前若有未退出声明照常 join |
| fork 无 exec | 句柄已失效 → -1（与其它 View\* 一致） |
| ENOTSUP | durable/fs/redis 后端与前端缺符号 → -2；runtime 视为“无 view 快路径”，不提升、不 Q2、不 Ack |

**四仓 ABI 快照登记（同一 PR）**：新增 `kvspaceViewAck`、`kvspaceViewQuiesce`，
删除 `kvspaceViewRelease`，必须同步到

1. `kvspace`：`include/kvspace/kvspace.h` 原型 + `src/frontend.c` 可选 dlsym/trampoline；
2. `kvspace-c`：导出函数 + `nm -D` 符号快照（compare 基线）；
3. `kvspace-durable`：`ffi.rs` 两个显式 -2 stub；
4. `kvlang`：spec ABI 文档中的符号表（与 runtime 实际调用的符号一一对应）。

### 5.2 读路径映射（runtime/src/slotview.c:223-265）

| 现状 | v2 |
|---|---|
| 提升 = `kvlangBodyCapture`（裸指针） | `kvspaceViewResolve` / `ResolveFrom` 填 token |
| 每 512 命中 `kvspaceResolveRef` + `memcmp(ref)`（:203-217） | `kvspaceViewCheck`；STALE → `ViewRefresh` 原地刷新，失败才 demote |
| 读取直接返回 head/body | 不变（只提升 ≤8B `FIXED_SMALL`，单条原子 load） |
| N=8 懒提升 | 不变 |

> 边界：>8B body 继续走字符串 ABI 拷贝路径，不纳入 view 快路径。

### 5.3 静默点（Q1/Q2）放置

| 时机 | 动作 |
|---|---|
| 每 **64 条指令**（`kvcpu.c` 主循环，handler 返回后） | Q1：`view_epoch` 变了 → 作废读 view → `ViewAck` |
| 进入阻塞/长调用前（`kvspaceWatch`、sleep、把控制权交扩展） | 作废全部读 view → `ViewQuiesce(h,1)`；返回后 `Quiesce(h,0)` 并重新解析 |
| 帧/进程销毁前 | 作废全部读 view（下一次 Q1 自然 ack） |
| 纯持有进程（open 后不跑 op） | 保持 claim 时的 `quiesced=1`，直到 close |
| runtime 首次使用 view 前 | 必须 `Quiesce(h,0)`（open 默认 1），否则任何 View\* 硬错误 |

**runtime-c 必须括号化的阻塞点**（漏一个 = 该进程只能靠 200ms 超时放弃）：

1. `kvlangKvspaceWatch`（runtime/src/kvspace.c:205 → 后端 `kvspaceWatch` 阻塞）；
2. `vthread·sleep`（runtime/src/rwir_vthread.c:83 `nanosleep`）；
3. 扩展 handoff 后由驱动线程进入 watch 等待（同第 1 条）；
4. 进程主循环转入空闲前（此后不再执行 op）。

`quiesced` 期间 backend 拒绝 View*，所以“忘记 `Quiesce(0)` 就继续用 token”会硬错误，
不会静默读已作废指针。

快速路径成本：无提升 view 时一个可预测分支；有 view 时一条 acquire load。
任何 epoch 变化才走真正的 backend 调用。

**禁止**：在操作中途（Check→读完之前）作废并 ack。解释器线程因此永远不会在
“拿着 token 读 body”时签出 ack；维护线程任何情况下都不签。

### 5.4 写路径

- 同尺寸热写（runtime-c）：保留 A 的**裸 memcpy**（写槽不带 token）。promoted view
  只覆盖 ≤8B FIXED_SMALL；8B 对齐 inline 写在 x86_64/arm64 是单条原子 store，读侧直接
  读 body 无撕裂。`value_gen` 只由**后端拷贝路径**维护（SetValue/SetPart/SetPartByRef/
  `ViewWrite`），与 promoted 集合不相交。
- `WriteInPlace` / `WriteNewPlace`：返回 body 指针、由调用方填充，**不承诺 value_gen**；
  只用于 >8B/容器/ext/PC status；view-cached ≤8B 槽**不得**经它们写；仍 one writer per key。
- 换尺寸/新键：字符串 ABI，后端内部 reserve/commit。
- 删除：`kvspaceViewDel`（reserve + CAS + commit）。

### 5.5 多线程 runtime（runtime-rs）

- 每线程独立 body slot 注册表；进程级 epoch 镜像全局。
- Q1：解释器线程持共享读锁，静默点取写锁 → 作废**所有线程**的读 view → `ViewAck`。
- Q2：进入阻塞前必须拿到全部线程的静默（全局写锁），再 `Quiesce(1)`。
- backend 不验证，完全依赖 runtime 实现这条锁；跨线程测试见 A3/A15。

---

## 6. 测试清单与验收

### A. 正确性

| # | 用例 | 验收 |
|---|---|---|
| A1 | tutorial shm 全量 ×3 | 226 PASS / 0 FAIL，三次一致 |
| A2 | conformance ×3 | 156/156 |
| A3 | 多进程压力：1 写者持续换尺寸（8.1%）+ 8 读者持 token 循环读并 quiesce，≥10⁷ op | 无崩溃；8B 读者只见 old/new；allocator 完整性（slotsbox 统计/double-free 检测）通过 |
| A4 | SIGSTOP 一个读者（非读窗口） | 屏障 200ms 放弃，garbage 不降，审计 +1；SIGCONT 后 ≤2 轮回收 |
| A5 | kill -9 读者 | 死槽不阻塞，一次超时内完成；槽回收；新进程可 open |
| A6 | leader 在消费后 / free 前 kill -9 | 下一 leader 接管；无 double free；`leak_boxes ≤ 1`；`garb_freeing` 有记录 |
| A7 | 64 槽满 | 第 65 个 open 失败且 stderr 精确；关一个后可 open |
| A8 | close/reopen（同进程） | 旧 token 必失效（构造 malloc 复用同一 `kv` 地址） |
| A9 | 环满（卡死读者 + 持续换尺寸） | 写硬错误 + 精确 stderr；无半写；`garb_bytes` 与已提交条目一致 |
| A10 | fork 无 exec | 子进程任何 `kvspaceView*` 返回 -1，不 hang |
| A11 | 两进程同删一键 | 只有一个 reserve+commit，另一个 STALE |
| A12 | 删除后立刻重插同键 | 旧 box 入环、新 box 独立；旧 token STALE；重插后读正确 |
| **A13** | **leader 当选时，另一进程解释器正处 Check→读同一 box**（测试构建用 `P2_TEST_HOLD_READ_MS` 在读窗口内停住，且该进程延后 ack） | **本轮必须放弃或等该进程 ack；读窗口内该 box 不得被 free（allocator 仍持有）；窗口结束后下一轮恰好 free 一次；读到的值完整** |
| **A14** | **在 A13 的读窗口内 SIGSTOP 该读者** | 屏障超时不 free；SIGCONT 后值完整、ack 后下一轮 free；期间 `round_aborted` 增长 |
| **A15** | **quiesce 阻塞进程参与轮次**：进程 A 阻塞在 watch 并已 `Quiesce(1)`，进程 B 产生垃圾并当选 | 本轮不等 A 的 op 边界即可完成；A 退出声明后重建 token 读值正确、无 UAF；A 未声明时同场景本轮放弃 |
| **A16** | **从不 ack、从不声明 quiesce 的占用者**（open 后挂起） | 每轮放弃 + 审计；垃圾增长到环满后写硬错误；该进程 kill 后 `ViewSweep` 一次清空环 |
| **A17** | **push 失败无半写**：填满环后做一次换尺寸 Set | Set 返回失败；旧值仍可读且逐字节不变；`box_offset` 未变；allocator 无新 box 泄漏；无 `RESERVED` 悬空条目 |
| A18 | 维护线程当选/等待期间，解释器在跑 | 解释器每 64 op 内 ack；维护线程**未**写任何 ack（用 P2_DEBUG 断言 ack 写入者 = 解释器线程） |
| **A19** | **reserve 后变更失败**：测试钩子让 `kv_sbo_alloc` / `art_ins` 失败 | 旧值逐字节不变；该条 `meta.state==EMPTY`（无 COMMITTED），`off` 未被当作状态使用；`tail` 前进但 `garb_bytes` 不变；后续一轮能越过；allocator 无新 box 泄漏；`reserved == committed + cancelled` |
| **A20** | **drain 不跨未决条**：测试钩子 `P2_TEST_HOLD_RESERVE_MS` 把某写者卡在 reserve 与 commit 之间，其它写者在其后追加 | 卡住期间的轮次不得免费卡住条之后的任何条目（即使 `epoch < r`）；卡住条 commit 前 head 不越过；commit 后下一轮按序释放且每个 box 只 free 一次；`off=0` 的合法 box 不被误判为 EMPTY（构造 offset 0 的 box） |
| **A21** | **原子访问静态检查**：全仓 grep `has_value` / `box_offset` 直接位域访问 = 0 处，全部经 `art_val_*`；resolve 在并发写压力下不产生 (gen, box) 失配通过 Check（debug 断言 + 强压） | 检查脚本 0 命中；压力下无 token 读到已退役 box |
| **A22** | **新进程中途 open**（claim 后 `quiesced=1`，未跑任何 op） | 不阻塞正在进行的轮次；runtime 就绪调 `ViewQuiesce(0)` 后正常解析/使用；纯持有进程保持 1，轮次永不因它超时 |
| **A23** | **leader 自己进程**：维护线程当选，但同进程解释器既未 ack 也未声明静默 | 本轮超时且零 free；解释器 Q1 ack 或 Q2 声明后，下一轮成功回收 |
| **A24** | A 快路径同尺寸写 + view 读并发（多进程） | 读只见 old 或 new（8B 原子）；换尺寸后旧 token `Check` STALE、`Refresh` 后读到新值 |

### B. 性能（**≥17 核**；隔离 `$ORIGIN` bin；10 轮 repeat=3，只跑 shm）

| # | 指标 | 目标 |
|---|---|---|
| B1 | Get | p50 ≤50 cyc，p95 ≤60 |
| B2 | Set 同尺寸覆盖 | p50 ≤50，p95 ≤60 |
| B3 | Del | p50 ≤120，p95 ≤150 |
| B4 | ResolveFrom | ≤300（去 pin 后预期 ~206/170，需实测） |
| B5 | 屏障时延 | 1 进程 ≤100µs；2/4/8/16 进程 p95 ≤1ms |
| B6 | 一轮回收吞吐 | 932k 条 ≤50ms（后台，不影响前台） |
| B7 | 10-case shm A/B | 回归 ≤2%；iops 不退步（A 落地后 301 ticks） |
| B8 | ack 开销 | 每 64 op 一次检查；iops 端到端 ≤1% |

> B5/B6 在 2 vCPU bj-build 上测不出真值（CFS 调度支配），必须换机。
> **发布前置**：A13/A19/A20/A21 必须在 arm64（Mac mini）跑通；x86_64 通过只算阶段性。

---

## 7. 风险与未决

1. **单写者/每 store 契约**：ART 结构更新（`art_add`/`art_grow`/`art_resize`/`art_del`）
   没有跨进程锁（只有 SBO 分配器有共享锁）。两个进程并发写——**即使不同 key**——也会在
   共享祖先节点上丢更新（实测：put 成功后紧接 del 认为键不存在，或崩溃）。同键并发写还
   会双退役。契约必须写死：**一个 store 同一时刻只有一个写者进程**；跨进程写者序列化
   单开 issue（不改 50c 快路径）。
2. **长 native op / 卡死进程**：不声明 quiesce 且不 ack → 每轮放弃；持续则环满后写硬错误。
   是否加“卡死告警阈值”留给评审。
3. **RESERVED 悬空**：预留者在 reserve→commit 之间崩溃/被 SIGSTOP。死亡 → 取消+审计；
   存活卡住 → drain 严格停在该条（A20），后面的条目一律不动，环满后硬错误。
   窗口只有几条指令，但拒绝一切“跳过”来换取吞吐。`claim_id` 30 位、发号到上限时
   **open 硬失败**（不静默回绕），见 §1.3。
4. **多线程 runtime 的全局静默**：backend 无法验证，依赖 runtime-rs 的写锁；需跨线程测试。
5. **32 位 epoch**：模比较，200ms 一轮 → 27 年回绕。
6. **macOS**：无回收语义。
7. **弱内存**：本机是 x86_64，A21 只能做访问路径静态检查与强压；真正的乱序验证要在
   arm64（Mac mini，正好是新 ABI 落地的机器）上跑 A13/A19/A20/A21。
8. **性能结论待 Mac mini 新 ABI 落地后复测**。

---

## 8. 供评审的实现切分（顺序由你定）

1. gen 表瘦身 + 删 pin + store_id 修正；
2. hdr v5 + `art_val_*` helper 改造（A21 静态检查）+ `.garb` reserve/commit/cancel
   + 三个写入点（暂不回收）；
3. 屏障轮 + Q1/Q2 + futex + 维护线程（单进程先行）；四仓 ABI 快照同步；
4. runtime 静默点接入（64 op + 四个阻塞点括号化）+ 读路径换 View\*；
5. 多进程：选举/接管/死槽/超时审计；
6. macOS 收口 + 全量回归 + 性能复测（arm64 上补 A13/A19/A20/A21）。
