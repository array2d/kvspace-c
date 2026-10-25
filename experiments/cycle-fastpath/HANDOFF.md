# HANDOFF：kvspace 50-cycle 快路径

> 交接日期：2026-10-25
> 接手方：Linux 侧 Claude
> 基线：kvspace-c `486e67e` = v0.3.16；kvspace `cc88806` = v0.3.16；kvspace-durable `342e7cd` = v0.3.16；slotsboxmalloc `b871d48` = v0.1.7；blockmalloc `43f57a6` = v0.1.5
> 所有材料在本目录（仓库内 `experiments/cycle-fastpath/`）；本目录是 source of truth。

## 1. 目标与验收

| 操作（public ABI） | 目标 | 当前（真实构建） |
|---|---|---|
| Get 命中 | median ≤50 cyc，p95 ≤60 | 21.9 ✅ |
| Set 覆盖 | median ≤50 cyc，p95 ≤60 | 43.8 ✅ |
| ResolveFrom(prefix,rel) | ≤300 cyc | 231.1 / 195.0（realistic/short）✅ |
| Del 前台 | median ≤120，p95 ≤150 | 98.6 / 131.5 ✅ |
| Set 新建 | ≤300 cyc | 未达成（~1700），单独目标 |

- key 档：realistic `/vthread/123456/[12]/t0`（23B，主验收）、short `/vthread/1/[1]/a`（16B）。
- 口径：Linux x86_64，`lfence;rdtscp`，依赖加法链标定（cycles/tick≈1.264）；`taskset -c 0` + `setarch -R`；3×30×20k；绝对成本只报公共 ABI 路径。
- 50c 仅覆盖 Get / 同尺寸 Set 覆盖；Del 是独立目标。

## 2. 当前实现状态

已完成（代码在 `p2/src/`、patch 在 `patches2/`）：
- packed64 CAS pin/unpin（DEAD + `PENDING && ref==0` 守卫）、`ViewRelease`；
- 叶子级逻辑墓碑（`has_value=0 + value_gen++`）、sweep 的 CAS 回收协议；
- 读路径 `recovery_epoch` 校验；struct_gen/value_gen 分离；
- 64 槽进程注册表（pid+starttime、先收死槽再认领、满则 open 失败）；
- 恢复屏障（recovery_epoch + ack + 超时审计）；macOS 无 /proc 走不回收分支；
- reset bug（`d==klen` 判断位置）已修；槽满 64/64 PASS×3；
- 同进程并发 5s：22.5M ops、bad=0、stale=20。

未完成 / 进行中：
- **静默屏障模型已采纳，尚未实现**（见 §3）；退役链（§1.5）已归档为备选，不实现；
- 真实 kvlang 换尺寸/复活写占比未测（唯一影响阈值结论的缺口）；
- 全套并发测试（SIGSTOP 不 ack、kill -9、屏障中崩溃、槽满、close/重开、泄漏）未跑；
- 四仓 patch 未最终拆分（kvspace 规范 / kvspace-c / kvspace-durable -2 桩 / runtime 接入方案）；
- P1 patch 未 rebase（①②③ 保留，④ 解码缓存废弃）；
- Get/Set per-op p95、持续删除吞吐、max 离群归因未量化。

## 3. 已确认的架构决策

1. **放弃退役链（refcount/代际 entry）**：崩溃语义状态过多（RETIRING 发布、4–5 孤儿、回滚 entry、双击 free）。归档在 `p2-reclaim-model.md §1.5`。
2. **采用静默屏障（quiescent barrier）**：token 不 pin，只带 `(struct_gen, value_gen, box, recovery_epoch)`；同尺寸就地覆写只 `value_gen++`；换尺寸/复活/删除的旧 box 记为垃圾；屏障推进 `recovery_epoch`，所有存活进程在操作边界失效 token 并 ack，全部 ack 后才 free；超时/卡住的活进程放弃本轮回收并审计，绝不强制 free；死进程不 ack 也可回收。
3. **prefix 对象可被 runtime 长期缓存**：固定大小、调用方持有；struct_gen 只跟踪结构，叶值写不使前缀失效；`ViewRelease` 由帧缓存管理。
4. **macOS 不启用屏障回收**（无 /proc）：墓碑不 free，`kvspaceClose` 统一释放；不要宣称并发回收语义。
5. **测量机与开发机分离**：bj-build 只做构建/cycle 测量；源码/patch/结果必须回本机（或本分支）。

## 4. 实测数据（供继续）

- 8B 值 box 物理容量 72B；64B 值 128B。
- 同尺寸覆写 203ns/op（4.9M op/s）；换尺寸 8↔64 239ns/op。
- 64MiB 垃圾阈值 ≈ 932,067 次换尺寸触发一次屏障。
- 屏障开销（8 进程）：换尺寸占比 100%→6.8%，10%→0.68%，1%→0.068%。
- 屏障 ack：1 进程 35µs；2 vCPU 上 ≥2 进程约 1.5ms（一个调度时间片）；16 进程 <1ms 需 ≥17 核机器复测。
- sweep DFS：1 键 5.8µs，4096 键 39µs。

## 5. 下一步（按顺序）

1. **测真实换尺寸占比**：在 `/private/tmp` 的 kvlang v0.3.16 副本里构建 runtime + 带计数后端，跑 tutorial，统计换尺寸/复活写比例、垃圾速率、屏障触发次数；>10% 回来重算阈值。
2. **实现静默屏障**：
   - 垃圾跟踪：墓碑走 ART DFS；覆写用 8B 偏移分段表（7.5MiB/64MiB 阈值），写清表覆盖路径与表满行为（提前触发屏障或 EAGAIN）；
   - 后台维护线程 + `barrier_busy` CAS 选举 + 死 owner 用 pid+starttime 接管；
   - futex/eventfd ack，替换 1ms 轮询；
   - runtime ack 契约：只在 op 边界 ack、ack 前失效全部 token；
   - 写完重测四项热路径。
3. **全套并发测试**：SIGSTOP 不 ack → 放弃本轮、零 free；kill -9（含死进程持有 token）；屏障中崩溃；槽满；close/重开旧 token 永久失效；泄漏/垃圾上限。
4. **真实仓库构建复测**：patch apply 到三仓，仓库 CMake；默认 Release 数字为验收；`-falign-*` 只作说明。
5. **四仓 patch 拆分**：kvspace 规范（token/prefix/ack 条款）、kvspace-c、kvspace-durable（新符号恒 -2，风格对齐已有 `kvspaceSlotView` ENOTSUP）、runtime 接入方案（把 `klangReadViewTry` 的 N/RECHECK 映射到 token Check）。
6. **P1 rebase**：1023B 拷贝、`sbo_allocated_size`、读路径全量校验三项保留；解码缓存废弃。
7. **指标补齐**：Get/Set per-op p95、持续删除吞吐（≈150 cyc/box）、max 离群归因。

## 6. 已知风险

- 2 vCPU 上屏障 ack 约 1.5ms；多进程/多核数字需在 ≥17 核机器复测，不能外推。
- 存活但不 ack 的进程会阻止回收（安全，垃圾增长，审计）。
- 分段垃圾表容量与覆盖路径要压力测试；表满策略必须明确。
- 无 pin 的读窗口依赖「ack 只在操作边界」；runtime 必须遵守，否则屏障可能释放正在读的 box。
- 跨二进制代码布局会带来 ±15–23% 绝对差异；消融必须用紧邻 prod 对照。
- 默认 Release 的 ResolveFrom 231/195 为验收数字。

## 7. 规则

- 所有工作基于最新 tag；开始前 `git fetch --tags` 核对。
- 源码与 patch 的 source of truth 在本目录/分支；bj-build 只留可丢弃副本。
- patch 在全套测试通过前不冻结、不合并。
- 论文工作已暂停，不在本交接范围。
