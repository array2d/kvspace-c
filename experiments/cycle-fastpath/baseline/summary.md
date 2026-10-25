# P0 精确基线 — kvspace 寻址快路径

口径与 cycle-audit 完全一致：bj-build（AMD EPYC 7K62，2 vCPU，gcc 11）、`lfence;rdtscp`
+ 5×8 展开加法链标定、`taskset -c 0`、`setarch -R`（关 ASLR）、单热 key、8B inline 值、
3 运行 × 30 批 × 20000 op。原始样本见 `raw.csv`（1620 行），环境见 `env.txt` /
`manifest.json`。**本轮未改任何库代码。**

## 1. 真实栈槽 key 形态（从 kvlang runtime 抽样，不是假设）

来源：`runtime/src/runtime.c:55-73`（vid 生成）、`runtime/src/keytree.c:20-26`（frame root +
`/`）、`runtime/src/rwir_func.c:63-105`（`ResolveWriteSlot` = `frame_root + "/" + name`）。

| 组成 | 真实取值 | 证据 |
|---|---|---|
| vid | **十进制自增序列号**（`/vthread/‥seq` 读取后 `%lld` 加一），1–19 位 | `runtime.c:55-72` |
| frame（stackid） | `[<depth>]`，depth 从 1 起；pc 是 `[<depth>]/[<irseq>,<j>]` | `keytree.c:74`、`keytree.c:116` |
| varname | 源码标识符，抽样 tutorial 全文：1–10 字符（众数 2–3，最长 `max_profit`） | `tutorial/**/*.kv` |
| 实测 key 样例 | `/vthread/27/[1]/[2,0]`（pc）、`/vthread/27/[1]/…callpc` | 用 kvlang 跑 `hello.kv` 到 shm 后 dump |

**与设计 §1.1 的差异**：设计写 “realistic = 16–36 字符 vid、38–58B key”，但本运行时
的 vid 是十进制计数（不产生 UUID）。因此给三档：

| 档 | key | 长度 |
|---|---|---:|
| short | `/vthread/1/[1]/a` | 16 B |
| realistic | `/vthread/123456/[12]/t0` | 23 B |
| design-assumed | `/vthread/0123…def/[3]/t0`（vid 36 字符） | 53 B |

`realistic` 取 6 位 vid（≈10⁶ 次 spawn）+ 4 字符 frame + 2 字符名；`design-assumed`
保留设计假设的长度，用于预算可比性。

## 2. 现状（ABI 字符串路径，ticks/op / core cycles）

| 操作 | short 16B | realistic 23B | design 53B |
|---|---:|---:|---:|
| Get 命中 | 383.4 / 487 | 423.0 / 535 | 494.2 / 625 |
| Get 未命中 | 386.9 / 489 | 406.0 / 513 | 535.1 / 676 |
| Set 覆盖 | 461.8 / 586 | 506.0 / 642 | 662.3 / 837 |
| Set 新建 | 1186.2 / 1500 | 1342.2 / 1697 | 1790.2 / 2263 |
| Del 存在 | 359.1 / 455 | 408.3 / 516 | 519.1 / 656 |

void 率 ≤3.3%（只有 3 个配置各丢 2–3 批），标定 1.257–1.269。**全部距 50 cycles 差
10–13 倍。**

## 3. 前缀定位的潜在收益（探针近似，不是正式 API）

用测试构建导出 `kvspaceProbeFindDir` / `kvspaceProbeGetRel`（前缀节点 + 相对路径，
边界落在 ART 压缩前缀内部时按 skip 对齐），同进程、同 store 对比：

| 形态 | ticks/op | core cycles | 相对根路径 |
|---|---:|---:|---:|
| 根路径 Get（原生，无 dispatch） | 413.2 | 522 | — |
| **prefix + rel（realistic，前缀 `/vthread/123456/[12]/`）** | **169.0** | **214** | **−59%** |
| 根路径 Get（ABI，含 dispatch） | 423.0 | 535 | — |
| prefix + rel（short，`/vthread/1/[1]/`） | 144.3 | 183 | — |

### 3.1 自然前缀边界

| 前缀 | 覆盖的 key | 省掉的段 | 剩余 rel | 实测/推算 |
|---|---|---|---:|---:|---|
| `/vthread/<vid>/` | `/vthread/<vid>/[d]/<name>` | 2 段（`vthread` 7B + vid 1–19B） | `[d]/<name>` | 每段 20–40 ticks（按字节 2.5–3.6） |
| 帧根 `/vthread/<vid>/[d]/` | 同上 | 3 段（+ `[d]` 3–4B） | `<name>` | **实测 −244 ticks（realistic）** |
| `/lib/<pkg>·<func>/` | 库内 rwir 与数据键 | 2 段（`lib` 3B + `pkg·func`） | 其余相对路径 | 未单独测（形状不同，P2 再测） |

> 设计里“每段 ~90 ticks”来自 cycle-audit 的 seglen=32（每段 33B）。真实栈 key 的段
> 只有 3–19 字节，**每段约 20–40 ticks**；帧根前缀实测省 244 ticks，主要来自
> “免 1023B 定长拷贝 + 免 resolve_fetch + 少走 20 字节”，不是 3×90。

**结论**：前缀单独能把 535 cycles 压到 214（省 321 cycles），但**到不了 50**——剩下
214 cycles 里 `sbo_allocated_size`（约 131）与 wire 头解析（约 111）仍在。这两块必须由
P1/P2 消掉。设计里“每段 ~90 ticks”是 seglen=32 的产物；真实栈 key 每段只有 2–7 字节，
实测**每段 ≈ 20–40 ticks**（短前缀省 244 ticks 来自“免 1023B 拷贝 + 免 resolve_fetch +
少走 20 字节”，不是段数 × 90）。

## 4. 与 §3.4 的 50-cycle 预算逐项对账

| 预算项 | 预算 (cyc) | 现状 | 可测性 |
|---|---:|---|---|
| 调用 + 快路径入口 | 8 | ABI dispatch 9 ticks ≈ **11 cyc**（cycle-audit 配对）；原生直调更低 | 已测 |
| ref/leaf gen 校验 | 6 | 尚无实现 | 不可测（P2 后） |
| 值访问（8B load/store） | 5 | 被 `read_tlv` 包住：`sbo_allocated_size`（131 cyc）+ wire 视图（111 cyc） | 只能给上界（消融测得） |
| 边界/类型/flags 检查 | 5 | 同上，含在 wire 视图里 | 只能给上界 |
| 写入路径 gen 递增与发布 | 8 | 尚无实现；当前覆盖写 642 cyc 里主要是 `shm_set_validated` + box 写 | 不可测（P2 后） |
| 返回值组装 | 4 | 计入调用开销 | 只能给上界 |
| 余量 | 14 | — | — |

**可达性判断**：Get 从 535 → 214（前缀）之后，必须再去掉 size 查询（131）与 wire 视图
（111），才能进入 ~40 cycles 的量级；也就是 50c 目标本质上是 **P1 的两项 + P2 的 ref
缓存**同时成立，单靠前缀不够。

## 5. set_new 建议目标

实测 `set_new` = 1342 ticks / 1697 cyc（realistic），构成：Del（408）+ Set 覆盖（506）
+ 额外插入/分配（≈428）。同口径下 slotsbox `sbo_alloc+free` 热对 = 120 ticks / 152 cyc。
其中固定成分（路径 + dispatch + wire）≈ 250 cyc，分配 ≈ 152 cyc，ART 插入/元数据 ≈ 剩余。

**建议目标：≤300 cycles（≈237 ticks）**，前提是：① 复用前缀定位；② 分配走热池
（不触发 blockmalloc）；③ 删除走 tombstone（不触发 punch）。若不做 punch 延迟，
64 KiB 以上的删除仍会是 0.9 ms 级，但那不属于本目标的 8B inline 形状。

## 6. 前缀对象的失效频率与常驻内存（估算）

**ART 结构事实**（`kvspace-c/src/kvspace.c`）：
- 节点在 N4→N16→N48→N256 增长时被标记 `ART_MOVED`，新节点拿新 id（`art_resize`，
  kvspace.c:620-648）；`art_follow` 最多追 8 跳（kvspace.c:517-528）。
- 位置无关节点：**只要某个节点的子数跨过 4/16/48/256 的边界，且该节点恰是前缀节点，
  prefix 的 node_id 就失效**。
- 删除不缩容（`art_del` 只清 has_value），所以删除不会引起 move；目录删除/重建会让整条
  路径的节点消失。

**对栈前缀 `/vthread/<vid>/[d]/` 的估计**：
- 子节点 = 该帧的局部变量（1–10 个，见 §1），跨 4 的倍数时最多 1 次 move → **每帧
  ≤1 次 refresh**；帧内操作数是几十到几百次 → **命中率 >99%**。
- 写入叶值**不应**使前缀失效：prefix gen 必须只跟踪“结构”（子集变化/节点搬迁），
  否则每次写都 refresh，等于没用。**这是设计需要明确的一条**（§3.1.1 目前写的是
  “写者改子树 → STALE”，若按字面实现会与写频率同频失效）。
- 常驻内存：`node_id(4) + gen(4) + depth(2) + flags(1) + store_uuid(16) + path(≤64)`
  ≈ **96 B/帧**；相对帧结构本身可忽略。

## 7. 未做 / 不可得

- 没有实现 prefix/ref API（按 P0 要求），prefix 数字来自测试构建探针，属于**上界**。
- `get_prefix` 只测了 Get；Set/Del 的前缀收益未单独测（同一条走查，差别在写/删尾巴）。
- `design-assumed` 档（36 字符 vid）不对应任何真实运行形态，仅用于预算比较。
- 多进程并发下的 gen 行为未测（P4）。
