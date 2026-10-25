# P2 回收协议 — 交接文档（2026-10-25，暂停开发）

> 接手方：Linux Claude。源码真相在本机该目录；bj-build 上只有可丢弃的构建/测量副本。
> 本次交接**不含任何未推送的仓库改动**：真实四仓（kvspace / kvspace-c / kvspace-durable / slotsboxmalloc）
> 保持干净，所有改动只存在于 `workspace/cycle-fastpath/p2/src/` 的两份工作树与 `patches2/*.patch`。

## 1. 当前状态

### 1.1 已达标数字（v0.3.16，公共 ABI，bj-build 实测，3×30 批）

| 操作 | p50 | p95 | 目标 | 结论 |
|---|---:|---:|---:|---|
| Get（ViewCheck + 读 body） | **21.9 cyc** | — | ≤50 / p95≤60 | ✅ |
| Set 覆盖（ViewWrite） | **43.8 cyc** | — | ≤50 / p95≤60 | ✅ |
| ResolveFrom | **231.1 / 195.0 cyc** | — | ≤300 | ✅ |
| Del（叶子墓碑） | **98.6 cyc** | 131.5 | ≤120 / ≤150 | ✅ |

DSO sha256 前缀：`09bba0dcc3b63e6f`（kvspace-c，真实构建）。原始数据见 `p2/results.md`、
`rebaseline/`、`baseline/`。

### 1.2 已完成模块（代码在 `p2/src/kvspace-c`，patch 已生成）

- packed64 CAS：`{gen:32|flags:2|dead:1|pending:1|ref:28}`；pin 同时校验 gen、拒绝 DEAD 与
  `PENDING && REF==0`；unpin 的 DEAD 0→1 唯一赢家执行 `sbo_free`。
- `ViewRelease`、sweep 的 CAS 回收协议、读路径 `recovery_epoch` 校验、复活 reset（CAS 循环）。
- **reset 竞态修复**：`p2_node_at` 的 `d==klen` 判断原来错放在 `if (h->prefix_len)` 内，
  键结束在无前缀节点时越界返回 -1，复活 reset 从未执行（观测值 `0x630000000` 的根因）。
  已移出；`del_tokens` K=1/4/64 多批回归无 STALE。
- **进程注册表**：64 槽 `{pid,starttime,recovery_ack,state}`；`/proc/<pid>/stat` 第 22 字段判活
  （ENOENT=死、不可判定=存活）；CLAIMING 中间态消除认领竞态；open 先收死槽再判满，
  第 65 个进程 open 失败并打印 `too many processes, N=64`；close 释放槽。
- **恢复屏障**：sweep 推进 `recovery_epoch`、标记死槽 RECOVERING、等存活槽 ack（短自旋 4096 次
  后 50µs 轮询，默认 200ms 超时）；超时不回收并审计；RECOVERING 遗留由下轮自愈。
- macOS：无 `/proc` 分支不判死、sweep 不 free（box 延迟到 close；close 统一释放**尚未实现**）。

### 1.3 未完成

1. **静默屏障本体**（architect 已裁决采用，模型见 §1.6）：token 去 pin、垃圾跟踪、后台维护线程、
   ack 契约、close 统一释放（macOS）。
2. **并发全套**：TSan/ASan、多进程（A sweep / B 读写 / C 只读）、kill -9、SIGSTOP、屏障中崩溃、
   close/重开、refcount 泄漏。已写的三个测试可直接用（见 §4）。
3. **四仓 patch 收尾**：durable 的 9 个 `kvspaceView*` ENOTSUP 桩（-2）、runtime 接入方案
   （token 帧缓存 + ack 契约）、patch 冻结。
4. **P1 ①②③ rebase**（1023B 拷贝 / `sbo_allocated_size` / 读路径全量校验）。
5. **Del 吞吐/墓碑堆积阈值**系统化；Get/Set 的 p95。
6. `art_del` 仍走 `gen_bump + kv_sbo_free_deferred`（ring+flusher 第二条 free 路径），
   在静默屏障落地时应统一（模型 §1.6.5）。

## 2. 关键决策与放弃退役链的原因

- 原模型（§1.5，**存档、不实现**）：按 view 引用计数 + `packed64` + 退役链/固定池。
  连续评审暴露：RETIRING 发布竞态、4–5 孤儿条目、回滚时 entry 复用 UAF、写者崩溃恢复、
  池上限与代际记账。每修一处又引入新的时序状态，崩溃语义不可控。
- 备选 B（每节点 4 退役槽）也被暂停；无锁单链 + entry 回收需要 hazard pointer/RCU。
- **最终选型（§1.6 静默屏障）**：token 不 pin；同尺寸就地写；换尺寸/复活/删除记垃圾；
  垃圾超阈值 → 推进 `recovery_epoch`，所有存活进程在操作边界 ack 后统一 free；超时放弃本轮。
  代价是屏障时延，收益是**无 refcount / 无退役链 / 无 RETIRING / 无双 free 状态机**，
  直接复用已实现的注册表 + 屏障。

## 3. 可行性实测（bj-build Linux，2026-10-25）

### 3.1 微基准（`bench/p2_barrier_probe.c`）

| 项 | 值 |
|---|---:|
| 8B 值的 box 物理容量 | 72B（body_cap 8）；64B 值 128B |
| 同尺寸覆写 | 203 ns/op（≈4.9M op/s） |
| 换尺寸覆写 8↔64 | 239 ns/op |
| 屏障 ack（1 进程） | 35 µs |
| 屏障 ack（2/4/8/16 进程，**2 vCPU**） | ≈1.5 ms（受 CFS 调度支配，进程数几乎不影响） |
| sweep DFS | 1 键 5.8 µs；4096 键 39.1 µs |

**注意**：bj-build 只有 2 vCPU。"16 进程 ack <1ms" 需 ≥17 核机器复测，本机数据不能作为该结论。

### 3.2 真实 kvlang workload 换尺寸占比（隔离副本 + 计数后端）

方法：`/private/tmp` 的 kvlang 仓库副本（`git archive HEAD`）构建 runtime/layout/runtime-rs，
链接本目录的 v0.3.16+P2 前端与带 `P2_SETSTATS` 计数的后端；跑 `tutorial/test.py --no-build`
（PASS 227 / FAIL 1，既有失败）。每进程取最终累计值，684 个进程聚合：

| 指标 | 值 |
|---|---:|
| 覆写写中同尺寸（可就地） | **81.5%** |
| 覆写写中换尺寸（需退役旧 box） | **18.5%** |
| 全部写中换尺寸 | **2.4%** |
| 全部写中新键（fresh，无垃圾） | 87.0% |
| 整轮套件垃圾字节 | ≈4.97 MB |

按 §1.6.2 的表：换尺寸占全部写 2.4% → 64MiB 阈值约每 8s 触发一次，
8 进程屏障开销 ≈0.16%；即便只算覆写（18.5%），开销 ≈1.3%。**均 <2%，模型可行。**
原始数据：`p2/writestat-perproc.txt`、`p2/writestat-summary.txt`、`p2/tutorial-suite.log`。

## 4. 文件清单与 sha256

### 4.1 源码（source of truth）

| 文件 | sha256 |
|---|---|
| `p2/src/kvspace-c/src/kvspace.c` | `659d6ab65d85f701355389c93e654d44595c8c5947d7864e971614ba8f19f1b6` |
| `p2/src/kvspace-c/src/kvspace_shm.h` | `3e8b9e0bae47a2b25f84de2a9345cec34692d4ac3687b1d4d32e68ca7702bbcf` |
| `p2/src/kvspace-c/src/xvalue_head.c` | `e3231008ed0129a10957ccf746674c2d021a4296c3eee4bcb0cd5087897971bf` |
| `p2/src/kvspace-c/src/xvalue_head.h` | `48870e212672f9b19556226f57898c05cd4ca88b1388eca814d1c3b739d2b469` |
| `p2/src/kvspace-c/src/durable_abi.c` | `18c8a7dcca197664325eb170646f7451e9e4ad8bf8c68f36d217d350be0f8b2d` |
| `p2/src/kvspace-frontend/include/kvspace/kvspace.h` | `1e10f59a0c5c0efdfeea1ff4c1e4f8de862f28279222c814893449e14f9d001d` |
| `p2/src/kvspace-frontend/src/frontend.c` | `a7e593889ace083a90d5e3bcc3b044d0ecf080eb1e622ae5319c14c29b3df47a` |

两份工作树的 git 基线均为 **v0.3.16**（kvspace-c `486e67e`、kvspace `cc88806`）；
`git -C p2/src/kvspace-c diff` 即本次全部改动。

### 4.2 patch / 文档 / 测试

| 文件 | sha256 |
|---|---|
| `patches2/kvspace-c-p2-reclaim.patch`（1240 行） | `2640bdfef15f490b3e893255a972b6766e631804a62e920673e1c193882c3fb1` |
| `patches2/kvspace-frontend-p2.patch`（130 行） | `2b70a56b876c39633da11b857e017dea437512fdfabec8a396f6eb57c8d020fd` |
| `patches2/README.md` | `bea422c18110afc8a4cf62acb8b33e79d2b64b0b493b8104150648f86b8960e1` |
| `p2-reclaim-model.md`（§1.5 存档 / §1.6 评估 / §6 状态） | `0f6270c78423a32b0f2f9c39cce14cac22f586c180b4a5edf1cad811dc117ffa` |
| `design.md` | `8952572ff6c301d0eb1c15e33cb096bf1e77206cda20d258cc4348cb1bea00ce` |
| `p2-design.md` | `ed6635305b3d723245d1d37f49aa801bdc8a09ea4172e30ee23b55c26d12ceca` |
| `bench/p2_threads_test.c` | `140bd94bdd7c8c5ead0a3095e8e77b47b5a4fc3483ba39c32574774cb05c213a` |
| `bench/p2_registry_test.c` | `d4a980d6266711ca2943d7c822217d8d2a017ae0f8f313034e50ed89e3f9fda0` |
| `bench/p2_barrier_probe.c` | `2e319ff8080a064bd5490b0e137eb16cae6e507bc81d44d0754abbb4ad5cee27` |
| `bench/p2_lifecycle_test.c` | `c0e1e165b5d18b313c4d4f17f72594b7ab55ae24c19081209e1a15e596dccf6f` |
| `bench/token_bench_abi.c` | `2310866349a57a1ba8d08be03d3a63685da263e308022d7587926e1ad7689154` |

数据：`p2/results.md`（历史 p50/p95）、`p2/writestat-perproc.txt`、`p2/writestat-summary.txt`、
`p2/tutorial-suite.log`、`baseline/`、`rebaseline/`、`p1/`。

### 4.3 临时探针（**不进入 patch**）

- `P2_DEBUG`：pin 失败 / reset / 节点查找 / 屏障超时 / 槽认领 的 stderr 打印。
- `P2_SETSTATS=<file>` + `p2_setstats_dump()` + `g_setstats`：写形态统计（本次评估用）。
- `bench/p2_barrier_probe.c`：box 容量、写路径成本、屏障时延、sweep DFS（测量专用）。
- `/private/tmp/kvt-p2/writestat.c`（macOS interpose 探针，未成功用上，可丢弃）。
- 清理 patch 时删除上述代码块即可；它们都受环境变量控制，不影响正常路径。

## 5. 构建与测量命令（Linux，v0.3.16）

### 5.1 三仓 + 两个 allocator

```bash
git clone --depth 1 -b v0.3.16 https://github.com/array2d/kvspace-c.git
git clone --depth 1 -b v0.3.16 https://github.com/array2d/kvspace.git
git clone --depth 1 -b v0.3.16 https://github.com/array2d/kvspace-durable.git
# 本目录提供的工作树（含 P2）可直接替换 kvspace-c / kvspace
patch -p4 -s < patches2/kvspace-c-p2-reclaim.patch      # 在 kvspace-c 根目录
patch -p4 -s < patches2/kvspace-frontend-p2.patch       # 在 kvspace 根目录
# 依赖：blockmalloc v0.1.5、slotsboxmalloc v0.1.7（header-only，含 include/）
cmake -S kvspace-c -B build -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_FLAGS="-I<blockmalloc>/include -I<slotsboxmalloc>/include" \
      -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build -j && cmake --install build
```

### 5.2 前端 + 测试

```bash
cmake -S kvspace -B build-front -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=<prefix>
cmake --build build-front -j && cmake --install build-front
export LD_LIBRARY_PATH=<prefix>/lib/kvspace
export KVSPACE_BACKEND_PATH=<prefix>/lib/kvspace
cc -O2 -I p2/src/kvspace-c/src -I <prefix>/include bench/token_bench_abi.c \
   -L <prefix>/lib/kvspace -lkvspace -o token_bench_abi
TOK_STORE=/tmp/a.dat TOK_K=64 ./token_bench_abi del_tokens /vthread/1/[1]/k0 3 1 100000
```

### 5.3 cycle 口径

bj-build：`taskset` 绑核 + `rdtscp`（`lfence;rdtscp;lfence`），依赖加法链标定，3 运行 × 30 批；
详细脚本见 `scripts/run_p*.sh`、`scripts/make_p*.py`。macOS 的 `cntvct_el0` 是 1GHz 虚拟计数器，
**不可用于 cycle 口径**。

## 6. 已知风险与阻塞点

1. **屏障时延依赖进程 ack 节奏**：卡住/长时间不 ack 的存活进程会让本轮屏障放弃（审计），
   垃圾持续增长；需要上限告警与（未来）强制策略。
2. **ack 契约是 runtime 的义务**：只在 op 边界 ack、ack 前失效全部 token、读 body 前后校验
   `recovery_epoch`。任何违反都会导致 UAF。
3. **2 vCPU 限制**：多进程 ack 时延 ≈1 调度时间片；16 进程目标需换机器。
4. **macOS**：不提供并发回收；close 统一释放尚未实现（当前 sweep 已 no-op，但 close 未扫）。
5. **art_del 旧路径**：`ring + flusher` 仍是第二条 free 路径，未与屏障统一。
6. **`p2_node_at` 之外的 walker**（`art_walk` 等）在 `ART_MOVED` 上直接返回 -1；本次未改动，
   若后续 token 使用遇到 MOVED 节点需复核。
7. **patch 未冻结**，且未在真实四仓落地；durable 桩与 runtime 方案缺失。

## 7. 目标验收

- Get ≤50 cyc（含 p95 ≤60）、Set-over ≤50（p95 ≤60）、ResolveFrom ≤300、
  Del ≤120（p95 ≤150）。
- 并发：TSan 零报告、ASan 无 UAF、多进程 kill -9/SIGSTOP/屏障中崩溃/槽满/close 重开/泄漏全过。
- 崩溃恢复：持有 token 的进程被 kill -9 后，屏障完成可回收其 pin 的 box。
- 新模型下的复测：四项热路径数字不退化；屏障频率与开销按 §3.2 的占比复核。

## 8. 建议的下一步（接手方）

1. 读 `p2-reclaim-model.md` §1.6（模型）与 §1.5（存档，仅供参考）。
2. 按 §1.6 实现静默屏障：去 pin → 垃圾表（8B 偏移分段，7.5MiB/64MiB）→ 墓碑走 ART DFS →
   后台维护线程 + `barrier_busy` CAS 选举 → runtime ack 契约。
3. 删掉 `ring + flusher`，`art_del` 统一为墓碑。
4. 跑 `bench/p2_threads_test.c`、`bench/p2_registry_test.c`（slots/orphan/sigstop）+ 新增
   TSan/ASan/多进程；再回 bj-build 复测四项。
5. 补 durable ENOTSUP 桩与 runtime 接入方案，冻结 patch。
