#!/usr/bin/env bash
# 校验 kvspace-c 自带的 xvalue_head 副本与上游 kvspace 逐字节一致。
#
# 为什么要副本：/usr/include 里的 kvspace 头由 deps.json 的 tag 决定，发版前拿不到
# 新布局；本仓 src/xvalue_head.{h,c} 是一份与上游逐字节相同的临时副本。
#
# 为什么必须校验：xvalue_head 是跨进程 ABI。副本一旦漂移，后端会按错误布局解释
# 前端写下的字节——能编过、能跑、读到的是错值，属于最危险的一类静默错误。
#
# 退出条件：上游发 tag 并把 deps.json 升上去之后，删掉副本、改为直接依赖上游头，
# 那时本脚本一并删除。
#
# 用法：ci/check_xvalue_head_sync.sh [上游 kvspace 仓库路径]
#   默认取 ../kvspace（与克隆目录约定 ~/{host}/{owner}/{repo} 一致）。
#   上游不在默认位置时（git archive 导出的树、隔离 CI、临时工作树），设
#   KVSPACE_CI_UPSTREAM 指向它的根目录。命令行参数优先级最高。
#
#   只指定「上游在哪」，不改变语义：证明不了一致就失败。

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if [ "$#" -gt 0 ]; then
    UPSTREAM="$1"
elif [ -n "${KVSPACE_CI_UPSTREAM:-}" ]; then
    UPSTREAM="$KVSPACE_CI_UPSTREAM"
else
    UPSTREAM="$ROOT/../kvspace"
fi

if [ ! -d "$UPSTREAM" ]; then
    echo "check_xvalue_head_sync: 找不到上游仓库 $UPSTREAM" >&2
    echo "  （这是硬失败：副本无法证明与上游一致时不得通过）" >&2
    echo "  用 KVSPACE_CI_UPSTREAM=<上游根目录> 指定位置。" >&2
    exit 1
fi

fail=0

check_file() {
    local vendor="$1" upstream="$2" strip_include="$3"
    if [ ! -f "$upstream" ]; then
        echo "check_xvalue_head_sync: 上游缺文件 $upstream" >&2
        fail=1
        return
    fi
    # .c 的唯一允许差异是 include 行（副本用相对路径，上游用 <kvspace/...>）。
    local normalized
    normalized="$(mktemp)"
    if [ "$strip_include" = "yes" ]; then
        sed '1s|.*|INCLUDE_LINE|' "$upstream" > "$normalized"
    else
        sed '1s|.*|INCLUDE_LINE|' "$upstream" > "$normalized"
    fi
    if ! diff -u "$normalized" <(sed '1s|.*|INCLUDE_LINE|' "$vendor") > /dev/null; then
        echo "check_xvalue_head_sync: 副本与上游不一致：$vendor" >&2
        echo "  若上游来自旧版本，请先确认两边版本一致再判断是哪一侧旧。" >&2
        diff -u "$normalized" <(sed '1s|.*|INCLUDE_LINE|' "$vendor") >&2 || true
        fail=1
    fi
    rm -f "$normalized"
}

check_file "$ROOT/src/xvalue_head.h" \
           "$UPSTREAM/include/kvspace/xvalue_head.h" no
check_file "$ROOT/src/xvalue_head.c" "$UPSTREAM/src/xvalue_head.c" yes

if [ "$fail" -ne 0 ]; then
    echo "check_xvalue_head_sync: FAIL" >&2
    exit 1
fi
echo "check_xvalue_head_sync: OK（副本与上游逐字节一致）"
