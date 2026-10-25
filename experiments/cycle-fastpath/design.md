# kvspace 寻址 ABI 50-cycle 快路径设计

> 日期：2026-10-24
> 目标：Get / Set 覆盖写 / Del 的命中热路径 ≤ **50 core cycles**（每操作，单线程、shm 后端、8B inline 值）
> 示例 key：`/vthread/{vid}/{stackid}/{varname}`（kvlang 栈槽，深度 4）
> 允许新增 ABI：resolve/handle 形态（用户已确认）
> 依据：`workspace/cycle-audit/report.md`

## 0. v0.3.16 重基线（2026-10-24）

P0/P1 跑在 kvspace-c/kvspace **v0.3.14**；上游已发布 **v0.3.16**（本地落后 20/10 个提交）。v0.3.16 已实现本设计的大部分方向：

- `kvspaceRef_t { block_id, gen, parent_id, depth }`：**parent_id + depth 就是可缓存的前缀对象**；`GetByRef` 从祖先节点续走剩余字节。
- `kvspaceResolveRef` / `kvspaceGetByRef` / `kvspaceSetValueByRef` / `kvspaceSetPartByRef`。
- `kvspaceSlotView`：直接返回 head/body 指针与物理容量，供每步直读/直写。
- `kvspaceGetHead`、`kvspaceGetPart/SetPart`、`kvspaceWriteInPlace`。
- 上游已有 `perf(shm): write SLACK part values without parsing the TLV head`、TLV 符号改名、移除 `kvspaceReadReset`。

因此：

1. P2 不再是“新增 ref/prefix ABI”，而是**测量并补齐现有 API 到 50c**；只有在现有语义不足时（例如不支持目录前缀 + 相对路径的稳定缓存）才提扩展。
2. P0/P1 的 v0.3.14 数据只作参考；必须在 v0.3.16 + slotsbox v0.1.7 + blockmalloc v0.1.5 上重跑。
3. `patches/kvspace-c-p1.patch` 先评估是否被上游覆盖，再决定保留/改写/废弃，不得盲目 rebase。
4. 50c 目标、key 口径、gen 语义（prefix 结构级 / leaf 值级）不变。
5. **任何阶段都基于最新 tag 开发**：每个阶段开始前 `fetch --tags` 核对 HEAD/tag；依赖用最新 tag；报告与 manifest 记录所有仓库的 HEAD/tag。上游升级后必须重跑受影响基线，不得沿用旧版本结论。

## 一、目标与边界

### 1.1 目标负载

P0 抽样后的真实形态（vid 是十进制自增序列号，不是 UUID）：

| 档 | key | 长度 | 用途 |
|---|---|---|---|
| short | `/vthread/1/[1]/a` | 16B | 次要档 |
| realistic | `/vthread/123456/[12]/t0` | 23B | **主验收档** |
| design-assumed | `/vthread/<36 字符 vid>/[12]/t0` | 53B | 仅作上界参考，不参与达标判定 |

- 单热 key、单线程、`shm://`、`taskset` 绑核；冷启动与首次 resolve 不计入。
- 每操作以 core cycles 计：bj-build `lfence;rdtscp` + 依赖加法链标定，口径与 cycle-audit 完全一致。
- 每段真实成本按 P0 实测 **20–40 ticks**（不是早期 seglen=32 的 ~90）。

### 1.2 达标范围（用户已确认）

- **≤50 cycles**：`Get` 命中、`Set` 覆盖已有值。按 realistic 23B 与 short 16B 两档验收，realistic 为主。
- **`Del` 独立目标 ≤120 cycles 前台 p50（p95 ≤150）**：tombstone + 结构级 gen + 入队，后台回收；后台回收的 CPU 摊销单列。理由：slotsbox free 热对约 152 cycles，同步回收不可能进 50；实测前台 floor 约 100–131。进一步降到 50 需要改 allocator 槽状态协议（外部 pending 位图 + sweep），列为后续独立任务，不在本轮。
- **set_new 目标 ≤300 cycles**（P0 实测 1697，构成：路径+dispatch+wire ≈250、分配 ≈152、ART 插入与元数据其余；前提是复用前缀、分配走热池、删除走 tombstone 不触发 punch）。
- **不在 50 cycles 内**：大值（>8B）、冷路径、首次 resolve、跨进程迁移。
- 冷路径允许比现状慢，但**不得使现有字符串 ABI 回归 >5%**。

## 二、现状与差距

cycle-audit 实测（ABI、单热 key、8B）：

| 形状 | ticks/op | core cycles |
|---|---:|---:|
| d1/s32 Get | 447 | 565 |
| d4/s32 Get | 753 | 952 |
| 栈 key（估） | ~600–750 | ~760–950 |

固定成本分解（d1/s32）：
- dispatch 9 ticks（≈11 cycles）
- 前缀逐字节比较 120 ticks（26%）
- `sbo_allocated_size` 取 shard 锁 104 ticks（23%）
- 定长 1023B 拷贝 79 ticks（17%）
- 未归因固定成本 ≤141 ticks（其中 wire 头解析上界 ~88）
- 值拷贝仅 7 ticks（Get 是借用返回）
- 大值额外：punch hole 2.34M ticks；UTF-8 全量重校验（64KiB 时 99.5%）

到 50 cycles（≈40 ticks）意味着要**同时消除**：路径解析、ART 走查、size 查询锁、key 拷贝、wire 头解码、每读校验，并把 dispatch 压到个位数。

## 三、达标所需的架构变化

### 3.1 新增 resolve/handle ABI

建议接口（名字与结构由实现者定，语义如下）：

```
kvspaceRef_t { leaf_id, gen, slot_off, box_size, value_len, flags, hash }
kvspaceRefResolve(kv, path, &ref)   // 一次：解析路径、走 ART、取 slot/gen
kvspaceRefGet(kv, &ref, &xv)        // 热路径：gen 校验 + 借用读
kvspaceRefWrite(kv, &ref, data,len) // 覆盖写：gen 校验 + 8B 原子写 + gen 递增
kvspaceRefDel(kv, &ref)             // 删除：gen 校验 + tombstone + 延迟回收
kvspaceRefRelease(kv, &ref)
```

- 字符串 ABI 保持不变，内部可由 resolve+ref 实现或维持原路径。
- durable 后端（kvspace-durable）必须给出同 ABI 的实现或明确的 `ENOTSUP`；kvspace 规范仓同步更新接口定义。这是四仓契约，按「4 库总是同时改」处理。

### 3.1.1 前缀对象（prefix handle）

除 leaf ref 外，所有寻址 API 增加一个「前缀对象」参数，避免每次从 root 重新走 ART：

```
kvspacePrefix_t { node_id, gen, depth, flags, norm_prefix }
kvspacePrefixResolve(kv, path, &prefix)            // 解析到目录节点（如 /vthread/<vid>/、帧根）
kvspaceGetP(kv, &prefix, rel, &xv)                 // rel 相对前缀，如 "[3,0]/t0"
kvspaceWriteP(kv, &prefix, rel, data, len)
kvspaceDelP(kv, &prefix, rel)
kvspaceListLenP(kv, &prefix, &n) / kvspaceListP(...)
```

约定：

- `rel` 不含前导 `/`，不允许 `..` 或空段；`rel == ""` 表示前缀自身。
- 前缀对象**允许被 kvlang runtime 长期缓存**（例如按 vthread/帧缓存，随帧销毁而释放），因此它必须是调用方持有的固定大小结构，不依赖调用栈生命周期、不由 kvspace 内部堆分配。
- `gen` 与所在 ART 节点/叶的 generation 比对，失效返回 `STALE`；`kvspacePrefixRefresh(kv, &prefix)` 用结构内保存的规范前缀路径重新解析，刷新后原对象继续可用，runtime 无需重新分配缓存槽。
- 结构内保存 **store identity**（后端 epoch/UUID）。`kvspaceClose`/重开后旧对象必须失效，防止跨 store 误用。
- 前缀对象只读；`refresh` 是唯一的写操作，需由调用方保证同一对象的刷新不与使用并发（或由 runtime 自行加锁）。
- 可跨线程共享（只读），不可跨进程传递（进程内句柄）。
- 语义必须与根路径 API 完全等价：同 key 的两种调用返回一致结果，随机对照测试覆盖。
- 组合关系：`prefix + rel` 先由前缀对象定位节点，再只对 `rel` 做路径解析；leaf ref 仍可进一步缓存最终槽位。两者可叠加，也可单独使用。
- 生命周期：resolve 后由调用方持有；不 pin 住 ART 节点；节点分裂/压缩通过 gen 检测，刷新即可，不要求重新 resolve 路径字符串。

**runtime 缓存模型（本设计的验收场景）**：

```
frame.prefix = resolve("/vthread/<vid>/[3,0]/")   // 帧创建时一次
loop:
  getP(&frame.prefix, "t0")  // 热路径；若 STALE → refresh 一次后重试
frame close: 丢弃对象（无需 release，对象由 frame 持有）
```

- 缓存命中时，每次操作只做：store identity 检查 + gen 比对 + 相对路径解析 + 槽访问。
- 缓存对象必须在帧/vthread 销毁时随宿主结构释放；不得跨 store 复用。
- runtime 侧的缓存策略（粒度、淘汰、refresh 频率）属于后续 kvlang runtime 任务；本设计只保证 ABI 允许这种缓存并给出 refresh 语义。

**预期收益**：栈 key `/vthread/{vid}/{stackid}/{varname}` 中，前缀 `/vthread/{vid}/{stackid}/` 固定，变化的是 `varname`。前缀对象把深度 4 的走查降为「前缀节点 + 1–2 段相对路径」，与 leaf ref 叠加后进入 50c 预算。

### 3.2 热路径必须满足的条件

| 成本项 | 现状 | 50c 方案 |
|---|---|---|
| 路径解析 | 每次逐字节 | resolve 一次；ref 只存 hash + 结果 |
| ART 走查 | 每次深度 4 | ref 缓存 leaf_id + slot 指针 |
| size 查询 | 每次 shard 锁 + 树下降 | box_size/value_len 在 ref 中；写入后不变 |
| key 拷贝 | 1023B 定长 | resolve 时按 `strnlen` 精确拷贝一次 |
| wire 头解码 | 每读一次 | resolve 时解码；ref 存 value_len/type/flags |
| 每读 UTF-8 校验 | 全量逐字节 | 写入时校验，leaf gen 担保；读路径只比对 gen |
| 锁 | 读路径仍有 shard 锁 | 读无锁；写用 leaf 级 gen + 原子 8B 存储 |
| punch hole | 删除同步 syscall | 延迟到后台/epoch 批量 |
| dispatch | 9 ticks | 专用快路径入口，目标 ≤8 cycles |

### 3.3 失效与并发模型

- **两级 generation**：
  - **leaf ref gen（值级）**：每次写入/删除目标槽递增；ref 使用前比对，不等则 `STALE`。
  - **prefix gen（结构级）**：只在子集变化（新增/删除子节点、ART 节点分裂/搬迁）时递增；**叶值更新不得使 prefix 失效**，否则前缀会与写频率同频失效而失去意义。P0 估算帧内 prefix 命中率 >99%，每帧 ≤1 次 refresh。
- **多进程**：gen 位于 shm，其他进程通过 ABI 写入会递增；ref/prefix 失效可检测。裸 mmap 篡改不在保证范围内（视为故障，不是支持路径）。
- **读**：seqlock 式校验（读 gen → 读值 → 再读 gen），不一致重试一次；8B 对齐值在 x86_64/arm64 上单次原子读写。
- **删除与回收**：先 tombstone + gen++，slot 释放延迟到 epoch 安全点；避免读者持有 ref 时 slot 被复用。
- **写覆盖**：8B inline 值直接原子写 + gen++；>8B 或需新分配 → 回落慢路径，不计入 50c。

### 3.4 50-cycle 预算草案

| 项 | 预算（cycles） |
|---|---:|
| 调用 + 快路径入口 | 8 |
| ref/leaf gen 校验（含 acquire） | 6 |
| 值访问（8B load/store） | 5 |
| 边界/类型/flags 检查 | 5 |
| 写入路径的 gen 递增与发布 | 8 |
| 返回值组装 | 4 |
| 余量 | 14 |
| **合计** | **50** |

任何一次 L2 miss（~14 cycles）或跨进程 cache line 争用都会吃掉余量；达标前提是 ref、leaf gen、value slot 在热态同处 L1。

## 四、实施阶段与验收

### P0 精确基线（必须先行）

- 用真实栈 key（short/realistic 两档）测 Get/Set-over/Set-new/Del 的现状：ticks/op、core cycles、阶段分解。
- 标出 kvlang runtime 中自然的**前缀边界**（vthread 根、帧根、`/lib/<pkg>·<func>/`），并量化「prefix + rel」相对根路径能省掉几段走查（按 cycle-audit 的每段 ~90 ticks 估算，给出区间）。
- 按 runtime 缓存模型估算：一帧内 prefix 的命中率、refresh 次数、以及 prefix 对象常驻内存（每帧一个）的规模。
- 验收：给出与 50c 预算逐项对应的现状表；set_new 的建议目标（基于实测）；前缀边界的候选清单与缓存命中估计。

### P1 无新 ABI 的内部压缩

- 去掉 1023B 定长拷贝（`strnlen+memcpy`）；
- 读路径去掉 `sbo_allocated_size`（wire 头长度 + 数据区上界）；
- 写入时校验 + leaf gen，读路径跳过 UTF-8 全量扫描；
- wire 头解码缓存（先做一个生产消融，收敛 88 的上界）。
- 验收：Get 降到 ≤250 cycles（预期），字符串 ABI 无回归；每项有消融数据。

### P2 resolve/ref + prefix ABI

- 实现 §3.1 的 leaf ref 与 §3.1.1 的 prefix handle；
- kvspace-c 先行；kvspace-durable 给 `ENOTSUP` 或同实现；kvspace 规范仓补接口条款；kvlang runtime 接入是后续独立任务。
- 验收：
  1. ref 热路径 Get/Set-over/Del ≤80 cycles；
  2. prefix+rel 与根路径结果等价（随机 key/值对照，含空 rel、多段 rel、非法 rel）；
  3. prefix 失效（gen 变化/ART 分裂）返回 STALE，调用方重 resolve 后结果正确；
  4. **runtime 缓存场景**：同一个 prefix 对象跨 ≥10k 次操作复用，期间另一进程写同一子树；STALE 检测、`refresh` 后继续使用、结果与根路径一致；
  5. store 关闭/重开后旧 prefix 对象必须失效（store identity 不匹配）；
  6. prefix 单独使用时，相比根路径的 cycle 差值被测量并记录（目标：省掉前缀段数的走查）。

### P3 压到 50 cycles

- ref 结构布局（单 cache line）、分支消除、gen 校验与值访问合并、避免多余 fence；
- prefix 与 ref 组合：前缀定位 + 相对路径解析 + 槽访问；
- 验收：Get / Set-over median ≤50 cycles、p95 ≤60；Del median ≤120、p95 ≤150；给出每项优化的 cycle 差值与代码位置；prefix 与 ref 各自的边际收益单独列。

### P4 并发正确性

- 多进程读/写/删压力：读者持 ref/prefix 期间写者修改 → 必须检测 stale 并重试；
- 删除后 slot 复用不导致读到新对象（epoch 回收）；
- prefix 指向的目录被删除/重建时，gen 必须失效；
- 验收：压力测试 + 断言；TSan/ASan（同进程）+ 多进程脚本；无静默错误。

### P5 文档与回归

- 更新 kvspace 规范、ABI 头文件注释、README；
- 全量锚定用例（228）在 shm/fs/redis 跑通；
- 冷路径与字符串 ABI 回归 ≤5%。

## 五、风险与明确不承诺

1. **50 cycles 只对热命中、8B inline、单写者/读多写少的场景成立**；跨进程高频写同一 leaf 会因 cache line 争用超标。
2. **set_new 不可能 50 cycles**：分配器热对已是 124–168 ticks（157–212 cycles），需要独立的 allocator 快路径设计。
3. **删除的 punch hole 必须移出关键路径**；否则 Del 永远达不到 50。
4. **校验信任模型变化**：读路径不再全量校验，安全依赖「写入方可信 + gen 失效」；需要单独安全评审。
5. **四仓契约**：新增 ABI 要同步 kvspace 规范、kvspace-c、kvspace-durable、kvlang runtime；不允许只改一个仓。
6. 不承诺 macOS 绝对 cycles；交叉验证只比值。

## 六、交付物

```
workspace/cycle-fastpath/
├── design.md（本文件）
├── baseline/（P0 数据）
├── results/（每阶段 CSV + 消融）
├── report.md（预算对账 + 达标结论）
└── patches/（P1/P2/P3 的 diff，按仓拆分，供评审后再落地）
```

实现代码先以 patch 形式提交评审；确认后再按四仓同步规则落地。
