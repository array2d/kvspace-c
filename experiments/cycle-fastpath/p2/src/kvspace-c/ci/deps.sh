#!/usr/bin/env bash
# 下载 ABI 依赖（deps.json: repo → tag），安装到 /usr/lib + /usr/include。本地与 CI 共用。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for repo in $(jq -r 'keys[]' "$ROOT/deps.json"); do
  ver=$(jq -r ".\"$repo\"" "$ROOT/deps.json")
  tmp="$(mktemp -d)"
  gh release download "$ver" -R "array2d/$repo" -p "${repo}-abi-*-linux-x86_64.tar.gz" -D "$tmp"
  tar xzf "$tmp"/*.tar.gz -C "$tmp" --strip-components=1
  if [ -d "$tmp/include" ]; then sudo cp -r "$tmp/include/"* /usr/include/; fi
  # blockmalloc / slotsboxmalloc 是 header-only：只发头、无 lib（实现体编进本仓 TU）
  if [ -d "$tmp/lib" ]; then sudo cp "$tmp/lib/"*.so* /usr/lib/; fi
  rm -rf "$tmp"
done
echo "✅ ABI deps → /usr/include（+ 有 lib 的仓装 /usr/lib）"
