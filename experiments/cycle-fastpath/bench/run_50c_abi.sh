#!/bin/bash
# run_50c_abi.sh — 50c 公共 ABI 基准（Get / Set 覆盖 / ResolveFrom / Del / del_tokens）
# 在参考机上直接跑：输出 p50/p95 与目标对齐表。Linux x86_64 已实测；arm64/macOS 见文末。
#
# 用法：
#   bench/run_50c_abi.sh                 # 构建后端+前端+bench，然后跑
#   KVB=/dir KVF=/dir bench/run_50c_abi.sh   # 用预构建的 backend/frontend 目录
#   ABI50_CPU=2 bench/run_50c_abi.sh       # 绑核（有 taskset 时）
#   ABI50_RUNS=3 ABI50_BATCHES=30 ABI50_OPS=20000   # 采样参数（默认即冻结口径）
#
# 输出：$WORK/50c-abi.csv + 终端汇总表。目标：Get ≤50/60、Set ≤50/60、
# ResolveFrom ≤300、Del ≤120/150（cycles，p50/p95）。
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=${ABI50_WORK:-/tmp/50c-abi}
RUNS=${ABI50_RUNS:-3}
BATCHES=${ABI50_BATCHES:-30}
OPS=${ABI50_OPS:-20000}
CAL=${ABI50_CAL:-100000}
mkdir -p "$WORK"

# ── 构建后端 / 前端 / bench（可用 KVB/KVF 覆盖）──────────────────────────
if [ -z "${KVB:-}" ]; then
    KVB="$WORK/backend"
    if [ ! -f "$KVB/libkvspace-c.so.1" ] || [ "${ABI50_FORCE:-0}" = 1 ]; then
        cmake -S "$HERE/p2/src/kvspace-c" -B "$KVB" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
        cmake --build "$KVB" -j >/dev/null || { echo "backend build failed"; exit 1; }
    fi
fi
if [ -z "${KVF:-}" ]; then
    KVF="$WORK/frontend"
    if [ ! -f "$KVF/libkvspace.so.1" ] || [ "${ABI50_FORCE:-0}" = 1 ]; then
        cmake -S "$HERE/p2/src/kvspace-frontend" -B "$KVF" -DCMAKE_BUILD_TYPE=Release >/dev/null &&
        cmake --build "$KVF" -j >/dev/null || { echo "frontend build failed"; exit 1; }
    fi
fi
BIN="$WORK/token_bench_abi"
if [ ! -x "$BIN" ] || [ "${ABI50_FORCE:-0}" = 1 ]; then
    cc -O2 -I "$HERE/p2/src/kvspace-frontend/include" -o "$BIN" \
       "$HERE/bench/token_bench_abi.c" \
       -L"$KVF" -lkvspace -ldl -Wl,-rpath,"$KVF" || exit 1
fi

PIN=""
if command -v taskset >/dev/null && [ -n "${ABI50_CPU:-}" ]; then PIN="taskset -c $ABI50_CPU"; fi
NOSLD=""
if command -v setarch >/dev/null; then NOSLD="setarch $(uname -m) -R"; fi

CSV="$WORK/50c-abi.csv"
RAW="$WORK/raw"
rm -rf "$RAW"; mkdir -p "$RAW"
R="/vthread/1/[1]/abcdefgh"   # realistic 23B
S="/vthread/1/[1]/x"          # short 16B
PFX="/vthread/1/[1]/"

run_op() { # op key prefix rel tag
    local op=$1 key=$2 pf=$3 rel=$4 tag=$5 run st
    for run in $(seq 0 $((RUNS-1))); do
        st="$WORK/stores/$tag-$run"; rm -rf "$st"; mkdir -p "$st"
        env TOK_STORE="$st/db" TOK_PREFIX="$pf" KVSPACE_BACKEND_PATH="$KVB" \
            $NOSLD $PIN "$BIN" "$op" "$key" "$BATCHES" "$OPS" "$CAL" "$rel" \
            >> "$RAW/$tag.csv" 2>/dev/null || { echo "$tag: bench failed"; return 1; }
    done
}

run_op get "$R" "$PFX" abcdefgh getR || exit 1
run_op get "$S" "$PFX" x        getS || exit 1
run_op set "$R" "$PFX" abcdefgh setR || exit 1
run_op set "$S" "$PFX" x        setS || exit 1
run_op resolve_from "$R" "$PFX" abcdefgh rfR || exit 1
run_op resolve_from "$S" "$PFX" x        rfS || exit 1
run_op from_ref "$R" "$PFX" abcdefgh frR || exit 1
run_op del "$R" "$PFX" abcdefgh delR || exit 1
run_op del "$S" "$PFX" x        delS || exit 1
for K in 64 4096; do
    for run in $(seq 0 $((RUNS-1))); do
        st="$WORK/stores/k$K-$run"; rm -rf "$st"; mkdir -p "$st"
        env TOK_STORE="$st/db" TOK_PREFIX="$PFX" TOK_K=$K \
            KVSPACE_BACKEND_PATH="$KVB" $NOSLD $PIN \
            "$BIN" del_tokens "$R" "$BATCHES" 1 "$CAL" \
            >> "$RAW/deltok$K.csv" 2>/dev/null || { echo "deltok$K: bench failed"; exit 1; }
    done
done

python3 - "$RAW" <<'PY'
import sys, os, glob, statistics, collections
raw = sys.argv[1]
vals = collections.defaultdict(list)   # tag -> [(p50, p95)]
def p95(v):
    v = sorted(v); return v[min(len(v)-1, int(len(v)*0.95))]
for f in glob.glob(os.path.join(raw, '*.csv')):
    tag = os.path.basename(f)[:-4]
    for line in open(f):
        p = line.strip().split(',')
        if len(p) < 11 or p[0] == 'op':
            continue
        if p[0] == 'del_tokens':
            cal = (float(p[6]) + float(p[7])) / 2
            vals[tag].append((int(p[11])*cal, int(p[12])*cal))
        else:
            cyl = float(p[9])
            vals[tag].append((cyl, cyl))
names = {'getR':'Get realistic','getS':'Get short','setR':'Set-over realistic',
         'setS':'Set-over short','rfR':'ResolveFrom realistic','rfS':'ResolveFrom short',
         'frR':'FromRef realistic','delR':'Del realistic','delS':'Del short',
         'deltok64':'Del tokens K=64','deltok4096':'Del tokens K=4096'}
targets = {'Get realistic':(50,60),'Get short':(50,60),'Set-over realistic':(50,60),
           'Set-over short':(50,60),'ResolveFrom realistic':(300,None),
           'ResolveFrom short':(300,None),'Del realistic':(120,150),
           'Del short':(120,150),'Del tokens K=64':(120,150),
           'Del tokens K=4096':(120,150)}
print(f"{'metric':<24}{'n':>4}{'p50':>9}{'p95':>9}{'target':>12}  verdict")
for tag in ['getR','getS','setR','setS','rfR','rfS','frR','delR','delS','deltok64','deltok4096']:
    if tag not in vals:
        continue
    name = names[tag]
    p50 = statistics.median(x[0] for x in vals[tag])
    p95v = p95([x[1] for x in vals[tag]])
    t = targets.get(name)
    ts = '-' if not t else (f'≤{t[0]}/{t[1]}' if t[1] else f'≤{t[0]}')
    if not t:
        verdict = 'record'
    else:
        ok = p50 <= t[0] and (t[1] is None or p95v <= t[1])
        verdict = 'PASS' if ok else 'FAIL'
    print(f"{name:<24}{len(vals[tag]):>4}{p50:>9.1f}{p95v:>9.1f}{ts:>12}  {verdict}")
PY
cat "$RAW"/*.csv > "$CSV" 2>/dev/null
echo "csv: $CSV"
# arm64/macOS：ticks() 用 cntvct_el0（aarch64）或 clock_gettime；无 taskset/setarch 时自动跳过。
# macOS 构建注意 SIP：前端/后端装到 /tmp，KVSPACE_BACKEND_PATH 指后端目录。
