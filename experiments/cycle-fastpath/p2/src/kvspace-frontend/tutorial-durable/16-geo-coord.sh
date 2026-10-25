#!/bin/bash
# 经纬度坐标：二维散 key 数组，坐标段 [lat,lng] 带小数点
# expected:
# === geo list ===
# [31.23,121.47]	char/utf8	Shanghai
# [39.9,116.4]	char/utf8	Beijing
# === get ===
# /geo·[39.9,116.4]	char/utf8:Beijing
# /geo·[31.23,121.47]	char/utf8:Shanghai
# /end

set -e
KV="$HOME/.local/bin/kvspace"
$KV deltree /geo/

echo "=== geo list ==="
$KV set /geo· 'map[0]:'   # 先声明容器（memhead），否则成员写被拒
$KV set '/geo·[39.9,116.4]' 'string:Beijing'
$KV set '/geo·[31.23,121.47]' 'string:Shanghai'
$KV list /geo· --kind --showext=false

echo "=== get ==="
$KV get '/geo·[39.9,116.4]' '/geo·[31.23,121.47]'
