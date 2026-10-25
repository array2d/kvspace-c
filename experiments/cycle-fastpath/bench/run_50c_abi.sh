#!/bin/bash
# run_50c_abi.sh — 50c 公共 ABI 基准（Get / Set 覆盖 / ResolveFrom / FromRef / Del / del_tokens）
#
# 单臂（默认）：构建 final 后端 + 前端 + bench，跑一轮套件。
#   bench/run_50c_abi.sh
# 双臂交替（baseline vs final，同机同前端同 bench，N 轮交替，推荐用于绝对对账）：
#   ABI50_ROUNDS=10 bench/run_50c_abi.sh
#   baseline = 实验分支 pre-step1 树（默认 4a8c7fb，p2 原型，含 View* 但无 Ack/Quiesce），
#   因为纯 v0.3.16 tag 没有 View ABI、跑不了本 bench。
# 发布回归列（不参与 50c 判定）：ABI50_V0316=1 时用已安装 v0.3.16 公共 ABI
# （kvspaceGet/SetValue/Del，无 View*）单跑 pget/pset/pdel，输出独立表。
# 可选：ABI50_CPU=2 绑核；ABI50_BASE=<预构建 baseline 后端目录>；KVB/KVF 复用构建产物。
#
# 输出：$WORK/50c-abi.csv + 目标对齐表 + （双臂时）baseline/final 对比表。
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(git -C "$HERE" rev-parse --show-toplevel 2>/dev/null || echo "$HERE")
WORK=${ABI50_WORK:-/tmp/50c-abi}
RUNS=${ABI50_RUNS:-3}
BATCHES=${ABI50_BATCHES:-30}
OPS=${ABI50_OPS:-20000}
CAL=${ABI50_CAL:-100000}
ROUNDS=${ABI50_ROUNDS:-1}
BASE_TAG=${ABI50_BASE_TAG:-4a8c7fb}
mkdir -p "$WORK"

# ── final 后端 / 前端 / bench ───────────────────────────────────────────
KVB=${KVB:-$WORK/backend}
if [ ! -f "$KVB/libkvspace-c.so.1" ] || [ "${ABI50_FORCE:-0}" = 1 ]; then
    cmake -S "$HERE/p2/src/kvspace-c" -B "$KVB" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    cmake --build "$KVB" -j >/dev/null || { echo "final backend build failed"; exit 1; }
fi
KVF=${KVF:-$WORK/frontend}
if [ ! -f "$KVF/libkvspace.so.1" ] || [ "${ABI50_FORCE:-0}" = 1 ]; then
    cmake -S "$HERE/p2/src/kvspace-frontend" -B "$KVF" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
    cmake --build "$KVF" -j >/dev/null || { echo "frontend build failed"; exit 1; }
fi
BIN="$WORK/token_bench_abi"
if [ ! -x "$BIN" ] || [ "${ABI50_FORCE:-0}" = 1 ]; then
    cc -O2 -I "$HERE/p2/src/kvspace-frontend/include" -o "$BIN" \
       "$HERE/bench/token_bench_abi.c" \
       -L"$KVF" -lkvspace -ldl -Wl,-rpath,"$KVF" || exit 1
fi

# ── baseline 后端（双臂时）──────────────────────────────────────────────
BASE_KVB=""
if [ "$ROUNDS" -gt 1 ] || [ -n "${ABI50_BASE:-}" ]; then
    if [ -n "${ABI50_BASE:-}" ]; then
        BASE_KVB=$ABI50_BASE
    else
        BASE_KVB="$WORK/base-backend"
        if [ ! -f "$BASE_KVB/libkvspace-c.so.1" ]; then
            rm -rf "$WORK/base-src"; mkdir -p "$WORK/base-src"
            git -C "$REPO" archive "$BASE_TAG" | tar -x -C "$WORK/base-src" || {
                echo "baseline archive $BASE_TAG failed"; exit 1; }
            cmake -S "$WORK/base-src/experiments/cycle-fastpath/p2/src/kvspace-c" \
                  -B "$BASE_KVB" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
            cmake --build "$BASE_KVB" -j >/dev/null || {
                echo "baseline backend build failed"; exit 1; }
        fi
    fi
fi

# ── 发布回归臂（v0.3.16 公共 ABI，无 View*）────────────────────────────
V0316=${ABI50_V0316:-0}
V0316_LIB=${ABI50_V0316_LIB:-/usr/lib/kvspace}
PLAIN_BIN="$WORK/token_bench_plain"
if [ "$V0316" = 1 ]; then
    # 前端在系统库路径（本机 /lib/libkvspace.so.1），后端在 V0316_LIB。
    if ! ldconfig -p 2>/dev/null | grep -q "libkvspace\.so\.1"; then
        echo "v0.3.16 frontend (libkvspace.so.1) not installed"; exit 1
    fi
    PL=()
    [ -f "$V0316_LIB/libkvspace.so.1" ] && PL=(-L"$V0316_LIB" -Wl,-rpath,"$V0316_LIB")
    cc -O2 -DABI_PLAIN_ONLY -o "$PLAIN_BIN" "$HERE/bench/token_bench_abi.c" \
       "${PL[@]}" -lkvspace -ldl || exit 1
fi

PIN=""
if command -v taskset >/dev/null && [ -n "${ABI50_CPU:-}" ]; then PIN="taskset -c $ABI50_CPU"; fi
NOSLD=""
if command -v setarch >/dev/null; then NOSLD="setarch $(uname -m) -R"; fi

R="/vthread/1/[1]/abcdefgh"   # realistic 23B
S="/vthread/1/[1]/x"          # short 16B
PFX="/vthread/1/[1]/"

# 一轮完整套件：$1=backend 目录 $2=raw 输出目录 $3=轮次
suite() {
    local bkv=$1 out=$2 round=$3 tag st run
    mkdir -p "$out"
    for spec in "get|$R|$PFX|abcdefgh|getR" "get|$S|$PFX|x|getS" \
                "set|$R|$PFX|abcdefgh|setR" "set|$S|$PFX|x|setS" \
                "resolve_from|$R|$PFX|abcdefgh|rfR" "resolve_from|$S|$PFX|x|rfS" \
                "from_ref|$R|$PFX|abcdefgh|frR" "del|$R|$PFX|abcdefgh|delR" \
                "del|$S|$PFX|x|delS"; do
        IFS='|' read -r op key pf rel tag <<< "$spec"
        for run in $(seq 0 $((RUNS-1))); do
            st="$WORK/stores/$round-$tag-$run"; rm -rf "$st"; mkdir -p "$st"
            env TOK_STORE="$st/db" TOK_PREFIX="$pf" KVSPACE_BACKEND_PATH="$bkv" \
                $NOSLD $PIN "$BIN" "$op" "$key" "$BATCHES" "$OPS" "$CAL" "$rel" \
                >> "$out/$tag.csv" 2>/dev/null || { echo "$tag: bench failed"; return 1; }
        done
    done
    for K in 64 4096; do
        for run in $(seq 0 $((RUNS-1))); do
            st="$WORK/stores/$round-k$K-$run"; rm -rf "$st"; mkdir -p "$st"
            env TOK_STORE="$st/db" TOK_PREFIX="$PFX" TOK_K=$K \
                KVSPACE_BACKEND_PATH="$bkv" $NOSLD $PIN \
                "$BIN" del_tokens "$R" "$BATCHES" 1 "$CAL" \
                >> "$out/deltok$K.csv" 2>/dev/null || { echo "deltok$K: bench failed"; return 1; }
        done
    done
}

# v0.3.16 发布回归：pget / pset / pdel（mean cycles，非 50c 口径）
suite_plain() {
    local out=$1 round=$2 op tag run st
    mkdir -p "$out"
    for spec in "pget|pget" "pset|pset" "pdel|pdel"; do
        IFS='|' read -r op tag <<< "$spec"
        for run in $(seq 0 $((RUNS-1))); do
            st="$WORK/stores/$round-$tag-$run"; rm -rf "$st"; mkdir -p "$st"
            env TOK_STORE="$st/db" TOK_PREFIX="$PFX" KVSPACE_BACKEND_PATH="$V0316_LIB" \
                $NOSLD $PIN "$PLAIN_BIN" "$op" "$R" "$BATCHES" "$OPS" "$CAL" \
                >> "$out/$tag.csv" 2>/dev/null || { echo "$tag: bench failed"; return 1; }
        done
    done
}

rm -rf "$WORK/raw"; mkdir -p "$WORK/raw"
if [ -z "$BASE_KVB" ]; then
    for r in $(seq 1 "$ROUNDS"); do suite "$KVB" "$WORK/raw/final" "$r" || exit 1; done
else
    for r in $(seq 1 "$ROUNDS"); do
        suite "$BASE_KVB" "$WORK/raw/base" "$r" || exit 1
        suite "$KVB"      "$WORK/raw/final" "$r" || exit 1
        if [ "$V0316" = 1 ]; then
            suite_plain "$WORK/raw/v0316" "$r" || exit 1
        fi
        echo "round $r done"
    done
fi

python3 - "$WORK/raw" "$BASE_KVB" "$V0316" <<'PY'
import sys, os, glob, statistics, collections
raw, has_base = sys.argv[1], sys.argv[2] != ''
has_v316 = sys.argv[3] == '1'
def p95(v):
    v = sorted(v); return v[min(len(v)-1, int(len(v)*0.95))]
def load(arm):
    vals = collections.defaultdict(list)
    for f in glob.glob(os.path.join(raw, arm, '*.csv')):
        tag = os.path.basename(f)[:-4]
        for line in open(f):
            p = line.strip().split(',')
            if len(p) < 11 or p[0] == 'op':
                continue
            if p[0] == 'del_tokens':
                cal = (float(p[6]) + float(p[7])) / 2
                vals[tag].append((int(p[11])*cal, int(p[12])*cal))
            else:
                cyl = float(p[9]); vals[tag].append((cyl, cyl))
    return vals
names = {'pget':'[v0.3.16] Get (plain)','pset':'[v0.3.16] Set-over (plain)',
         'pdel':'[v0.3.16] Del (plain)','getR':'Get realistic','getS':'Get short','setR':'Set-over realistic',
         'setS':'Set-over short','rfR':'ResolveFrom realistic','rfS':'ResolveFrom short',
         'frR':'FromRef realistic','delR':'Del realistic','delS':'Del short',
         'deltok64':'Del tokens K=64','deltok4096':'Del tokens K=4096'}
targets = {'Get realistic':(50,60),'Get short':(50,60),'Set-over realistic':(50,60),
           'Set-over short':(50,60),'ResolveFrom realistic':(300,None),
           'ResolveFrom short':(300,None),'Del realistic':(120,150),
           'Del short':(120,150),'Del tokens K=64':(120,150),
           'Del tokens K=4096':(120,150)}
order = ['getR','getS','setR','setS','rfR','rfS','frR','delR','delS','deltok64','deltok4096']
final = load('final')
v316 = load('v0316') if has_v316 else {}
base = load('base') if has_base else {}
hdr = f"{'metric':<24}{'n':>4}{'p50':>9}{'p95':>9}{'target':>12}  verdict"
if base:
    hdr += f"{'base p50':>10}{'base p95':>10}{'delta':>9}"
print(hdr)
for tag in order:
    if tag not in final: continue
    name = names[tag]
    p50 = statistics.median(x[0] for x in final[tag])
    p95v = p95([x[1] for x in final[tag]])
    t = targets.get(name)
    ts = '-' if not t else (f'≤{t[0]}/{t[1]}' if t[1] else f'≤{t[0]}')
    if not t: verdict = 'record'
    else:
        ok = p50 <= t[0] and (t[1] is None or p95v <= t[1])
        verdict = 'PASS' if ok else 'FAIL'
    line = f"{name:<24}{len(final[tag]):>4}{p50:>9.1f}{p95v:>9.1f}{ts:>12}  {verdict}"
    if base and tag in base:
        b50 = statistics.median(x[0] for x in base[tag])
        b95 = p95([x[1] for x in base[tag]])
        line += f"{b50:>10.1f}{b95:>10.1f}{(p50/b50-1)*100:>8.2f}%"
    print(line)
if has_v316 and v316:
    print("\n发布回归（v0.3.16 已安装公共 ABI，plain Get/SetValue/Del，mean cycles；"
          "口径不同，不参与 50c 判定）")
    print(f"{'metric':<24}{'n':>4}{'mean':>9}{'p50':>9}")
    for tag in ('pget', 'pset', 'pdel'):
        if tag not in v316: continue
        vals = sorted(x[0] for x in v316[tag])
        print(f"{names[tag]:<24}{len(vals):>4}"
              f"{statistics.mean(vals):>9.1f}{statistics.median(vals):>9.1f}")
PY
cat "$WORK/raw"/*/*.csv > "$WORK/50c-abi.csv" 2>/dev/null
echo "csv: $WORK/50c-abi.csv"
