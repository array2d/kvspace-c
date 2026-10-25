# P2 短设计：版本化 view token（待评审，未实现）

> 基于 v0.3.16（kvspace-c 486e67e / kvspace cc88806）。目标：把 rebaseline 里那条
> **2 cycles 的缓存视图热路径**变成**可安全长期缓存**的形态，其余按 design.md §3.1.1。

## 1. 缺口（rebaseline 实测）

| 现状 | cycles | 问题 |
|---|---:|---|
| `ResolveRef`（每次） | 231 | 只能给 leaf/祖先节点 id，`gen` 被复用为 depth |
| `GetByRef`（缓存 leaf ref） | 246 | 每调用仍做 dispatch + `kv_sync` + `read_value` 全量校验 |
| `SlotView` 一次 + 直读 | **2** | 快，但**无任何失效检测**（借用指针，寿命到写/删/close） |
| `SlotView`（每次调用） | 416 | 每 op 重做校验 |
| parent+depth 前缀 | 331 | short 键（depth==klen）直接失败 |

结论：**P2 不新造查找结构**，而是给现有的直接指针加版本头；缺的是
`struct_gen` / `value_gen` / `store_id` 三样东西。

## 2. 接口（新增符号，不改 `kvspaceRef_t` 布局，`kvspaceSlotView` 原样保留）

```c
typedef struct {
    /* 沿用 SlotView 的指针部分 */
    uint8_t *head;          /* XValue 起点（shm 映射内） */
    uint8_t *body;          /* head + headlen */
    uint32_t headlen;
    uint32_t value_len;     /* 解析时的逻辑长度；写后由 writer 更新 */
    uint64_t cap;           /* 整 box 物理容量 */
    uint64_t body_cap;
    /* 版本化部分 */
    uint32_t struct_gen;    /* 结构版本：子集变化 / 节点搬迁 / 目录删重建才变 */
    uint32_t value_gen;     /* 值版本：每次值写 / 删除递增 */
    uint64_t store_id;      /* 句柄身份：open 时分配，close/重开后旧 token 永久失效 */
} kvspaceViewToken_t;

/* 首次解析：等价于 ResolveRef + SlotView，但一次完成并填 token。 */
int kvspaceViewResolve(void *h, const char *key, kvspaceViewToken_t *tok);

/* 前缀形态：从已解析的父 ref（或上一个 token 的 node 部分）出发，rel 无前导 '/'
 * 无 '..'、空串表示前缀自身。用于「同一帧的多个变量槽」。 */
int kvspaceViewResolveFrom(void *h, const kvspaceRef_t *parent, const char *rel,
                           kvspaceViewToken_t *tok);

/* 原地刷新：按调用方保存的 key / (parent, rel) 重新解析，刷新后同一 token 继续用，
 * runtime 不需要换缓存槽。 */
int kvspaceViewRefresh(void *h, kvspaceViewToken_t *tok, const char *key);
int kvspaceViewRefreshFrom(void *h, kvspaceViewToken_t *tok,
                           const kvspaceRef_t *parent, const char *rel);

/* 热路径使用：调用方直接读 tok->body；写前用 kvspaceViewCheck 校验版本。 */
int kvspaceViewCheck(void *h, const kvspaceViewToken_t *tok); /* 0=有效, 1=STALE */
```

返回码：`0` 成功；`1` STALE（调用方 refresh 或回退字符串 ABI）；`-1` 不存在/非法参数；
`-2` 后端不支持（durable 显式 ENOTSUP，调用方回退）。

## 3. gen 存哪（**裁决：方案 B 旁路表**）

**A. 扩展 ART 节点头（已否决）**
`art_hdr_t` 增加两个 `uint32_t`：`struct_gen`、`value_gen`。
- 节点 +8 B（N4 头约 48 B → 56 B；ART 内存 +~4%）。
- 语义：叶节点的 `value_gen` 每次值写/删 +1；内部节点的 `struct_gen` 在**子集变化**
  （art_add/art_del）与**搬迁**（art_resize 生成新节点）时 +1；新节点继承旧 gen +1。
- 需要格式版本号（`kvspace_hdr_t.magic` 或新增 `format_version`）：旧 store 要么拒绝
  打开并提示重建，要么走一次性迁移（遍历 ART 补 gen=0 并置位版本）。
- 命中判定：token 的 `struct_gen` 与该节点当前值相等；`value_gen` 用 seqlock。

**B. 旁路 gen 表**：ART slab 头部加 `uint32_t gens[max_nodes]`，按节点 id 索引。
- 不改节点布局，但要为 slab 预留/增长一张并行表，节点 id 复用与表增长要一起处理；
  热路径多一次 `base + id*4` 的独立 load（多一条 cache line）。
- 迁移：老 store 的 gens 全 0，可直接使用（等价于“全部过期”，第一次 check 会 STALE 一次）。

**裁决（2026-10-24）**：选 **B**，不改 ART 节点布局。理由：A 每个节点 +8B、ART 内存约 +4%，
还要动持久化 payload 与迁移；B 只新增一个区域。热路径多一次 L1 load 可接受，**但必须实测
它是否把 per-op 推过 50；推过就回来重新裁决**。

**落地前核对（已完成）**：
- `art_hdr_t` = `{u8 type, u8 prefix[10], u8 prefix_len, u8 has_value:1 + u64 box_offset:63, u16 count}`
  —— 没有可用的 4B spare；改它就要动布局。
- `sbo_box_t` 只有一个显式 `uint8_t _pad`（每 box 一个字节，不是每 slot），放不下 per-value gen。
- 因此 `struct_gen` 与 `value_gen` **都走按 node id 索引的旁路表**（8B/节点：两个 u32）。
  节点 id 来自 `blocks_alloc`，是稠密的；表随 `art_slab_grow` 一起增长。
- **id 复用保护**：`art_balloc` 分配新节点时把 `struct_gen` 置为全局单调 epoch（而不是 0），
  这样被复用的 id 一定得到更大的 gen，旧 token 不会误命中。
- `value_gen` 不放进 XValue payload；同尺寸就地写只 bump `value_gen`。
- 旁路表属新区域，**不要求双后端 byte-identical**（它不是 wire 格式）；需要 hdr 里的
  `format_version`，旧 store 打开时把 gens 全置 0 并置位版本（等价“全部过期”，首次 check 会 STALE 一次）。

**旁路表布局（实现形态）**：`<path>.gen` 是**独立 mmap 的连续数组**，不是 KV 侧键——
open 时 `open()+ftruncate()+mmap(MAP_SHARED)`，`gens[id]` 就是 `base + id*8`，**一次 L1 load，
没有任何查找**。容量按 `art_slab / (sizeof_block_head + block_size)` 计算，随 `art_slab_grow`
一起 `ftruncate` 扩展（VA 预留 64 MiB）。node id 来自 blockmalloc，从 0 稠密分配；
`art_balloc` 分配时把 `struct_gen` 置为 hdr 里原子递增的全局 epoch，保证 id 复用不会误命中。
```
kvspace_hdr_t 增加: uint64_t gens_off; uint64_t gens_cap;  uint32_t format_version;
文件布局: [hdr][art slab][sbo head][sbo data][gens 区]   (gens 区随 slab 增长而扩展)
gens[id] = { uint32_t struct_gen; uint32_t value_gen; }
```

## 4. 失效与并发

| 事件 | struct_gen | value_gen | token |
|---|---|---|---|
| 同尺寸就地写（SLACK part write） | 不变 | +1 | 指针仍有效，`value_len` 由 writer 更新 |
| 值大小变化 / 重分配 | 不变（叶节点未动） | +1 | token 的 `head/body/cap` 可能已失效 → **换新 box 时 struct_gen 也要 +1**（见下） |
| 子集变化（加/删子节点） | +1 | — | 前缀 token 失效 |
| 节点搬迁（N4→N16…） | 新节点 +1 | — | 旧 token 失效（不追 MOVED 链，保持简单） |
| 目录删除/重建 | +1（或节点 id 变化） | — | 失效 |
| `kvspaceClose` / 重开 | — | — | `store_id` 不匹配，永久失效 |

> 修正：**重分配/换 box 时同时递增 struct_gen**（因为旧 token 的 `head/body/cap` 指向旧
> box）。这样“指针失效”与“结构版本”用同一个信号，读者只需比 struct_gen。
> 同尺寸就地写不改指针，只动 value_gen —— 与 design §3.3 的两级 gen 一致。

**读（seqlock）**：`v1 = value_gen(acquire); 读 body; v2 = value_gen(acquire); v1==v2 且为偶数`
才接受，否则重试一次后返回 STALE。8B 对齐值在 x86_64/arm64 上单次原子读写。
**写**：先 `value_gen++`（变奇，acquire/release），写 body，再 `value_gen++`（变偶）；
8B inline 值的写可以在同一个 8B store 内完成（gen 与值不在同一地址，需两条 store，
所以需要奇偶协议）。

**多进程**：gen 在 shm，别进程经 ABI 写入同样递增；裸 mmap 篡改不在保证范围（design §3.3）。
**store identity**：`store_id = hash(persisted_uuid, per-open_counter)`；同一文件两次 open
得到不同 id，旧 token 立即失效（不依赖文件内容）。
**只读**：token 是只读值类型，可跨线程共享；refresh/check 与写并发由调用方保证。

## 5. 验收口径（P2）

1. **热路径**（已缓存 token）：Get / Set-over / Del ≤ **50 cycles**，realistic 与 short 两档。
   - Get：check + 读 body（预期 ~10–20）
   - Set-over：check + value_gen 两次 store + 8B store（预期 ~20–30）
   - Del：check + value_gen + tombstone（**裁决：按“延迟释放 ≤50”验收**）。box 释放与
     punch 都推进 epoch 安全点；另设 `immediate release` 模式（实测 ≥152 cycles，单列，
     不与 Get/Set-over 同目标）。语义后果：空间延迟回收；读者不会看到悬垂数据
     （epoch 内不释放被引用过的 box）。
2. **首次 Resolve** 单独报（不计入 per-op）：
   - **裁决**：`ResolveFrom(prefix, rel)` 目标 **≤300 cycles**；root resolve 单独报，不计入；
     合并入口若实测 **>400 停下重新评估**，不硬凑。
   - 现状：ResolveRef 231 + SlotView 416；合并入口需实测。
3. **正确性**：token 与字符串 ABI 随机 key/值对照等价；STALE 后 refresh 结果一致；
   2 线程 + 2 进程写者下无静默错误；store 重开旧 token 失效。
4. **回归**：228 锚定用例 × shm/fs/redis 全通（P1 rebase 后一起跑）。

## 6. 与 P1 的关系

- ①②③（1023B 拷贝 / `sbo_allocated_size` / 读路径全量校验）**按 `read_value` 新落点
  rebase 后保留**，作为字符串 ABI 的低成本改进，与 P2 并行、patch 分开评审。
- ④ 解码缓存废弃（token 直接持有 head/body/value_len）。

## 7. 风险

- **ART 格式变更**（方案 A）：需要版本号与迁移/重建策略；跨版本 store 的兼容性要单测。
- **false sharing**：gen 与 `box_offset` 同 cache line，多写者高频写同一叶会放大争用；
  设计上接受（design §5 已明确 50c 只对读多写少成立）。
- **tok->value_len 的更新**：同尺寸就地写不改变长度；变长写会换 box 并 bump struct_gen，
  读者会 STALE 重解析，不依赖 writer 写长度字段。
- **short 键 depth==klen**：ResolveRefFrom 需要允许“前缀节点就是全部已消费字节”，
  即 `d == klen` 时直接返回该节点（现有 `ref_leaf` 的 `d >= klen` 返回 -1 要改）。
- **删除的全部成本**：Del ≤50 的前提是 tombstone + 延迟释放；若评审要求立即释放，
  Del 的目标必须改成 ≥alloc+free 热对（~152 cycles），不能与 Get/Set-over 同列。
