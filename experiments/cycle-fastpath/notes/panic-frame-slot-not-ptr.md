# 最小复现：`/vthread/<vid>/[1]/a is not a Ptr` panic

- 日期：2026-10-24
- 状态：**只记录，不在 cycle-fastpath 任务内修**（architect 另开单）
- 影响面：`kvlang -c` 走参数传递路径时 panic；`kvlang <file.kv>` 正常
- 复现（macOS，本机构建 bin/kvlang）：

```bash
KVSPACE_BACKEND_PATH=/tmp/kvs14/prefix/lib/kvspace \
KVSPACE=shm:///tmp/p0b.shm \
./bin/kvlang -c 'a = 1
b = a + 2
c = b * a
println(c)'
```

输出：

```
panic: /vthread/89/[1]/a is not a Ptr — frame slot missing, param passing broken
```

- 触发点：`runtime/src/rwir_func.c:70-74`（`ResolveWriteSlot` 的 `name[0]=='*'` 分支），
  读到 `/vthread/<vid>/[1]/a` 不是 Ptr 即 abort。
- 观察：`a = 1` 的写入没有把槽建出来（或用 `-c` 时 bootstrap 的参数帧未初始化）。
- 备注：`/tmp/p0b.shm` 的 vthread 1/2 只有 `[1]‥callpc`、`‥pc`、`‥status`，没有 `a`。
