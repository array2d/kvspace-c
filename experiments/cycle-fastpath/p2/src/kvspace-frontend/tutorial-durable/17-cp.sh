#!/bin/bash
# expected:
# === cp single key ===
# /dst/one	int64:100
# === cpdir memindex container ===
# x	float64	3.14
# y	int64	42
# === cpdir /-children subtree ===
# /d2/a	int64:1
# /d2/b	int64:2
# === cpdir extindex → new overlay on target ===
# /ov/a	int64:1
# /ov/b	int64:2
# === mutate ext, visible via copied overlay ===
# /ov/a	int64:777
# /end

set -e
KV="$HOME/.local/bin/kvspace"
$KV deltree /src/
$KV deltree /dst/
$KV deltree /d1/
$KV deltree /d2/
$KV deltree /base/
$KV deltree /realext/
$KV deltree /ov/

# 单 key 拷贝：值原样，不含成员
echo "=== cp single key ==="
$KV set /src/one int:100
$KV cp /src/one /dst/one
$KV get /dst/one

# cpdir 复制 · 成员容器（base + memindex + 成员）
echo "=== cpdir memindex container ==="
$KV set /p· 'map[0]:'   # 容器值（memhead）+ memindex；index: 只建 memindex，成员写会被拒
$KV set /p·x float:3.14
$KV set /p·y int:42
$KV cpdir /p /q
$KV list "/q·"

# cpdir 复制 /-children 子树
echo "=== cpdir /-children subtree ==="
$KV set /d1/ index:
$KV set /d1/a int:1
$KV set /d1/b int:2
$KV cpdir /d1 /d2
$KV get /d2/a /d2/b

# cpdir 遇 extindex：target 侧生成指向同一只读扩展的新 extindex
echo "=== cpdir extindex → new overlay on target ==="
$KV set /realext/ index:
$KV set /realext/a int:1
$KV set /realext/b int:2
$KV set /base/ index:
$KV extindex /base/ /realext/
$KV cpdir /base /ov
$KV get /ov/a /ov/b

# 只读扩展被改动 → 经拷贝出的 overlay 可见（未深拷贝缓冲）
echo "=== mutate ext, visible via copied overlay ==="
$KV set /realext/a int:777
$KV get /ov/a
