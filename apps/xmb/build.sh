#!/usr/bin/env bash
# build.sh host|device — сборка в WSL (root), лог в build/<цель>.log
cd "$(dirname "$0")"
mkdir -p build
make -j"$(nproc)" "$1" > "build/$1.log" 2>&1
rc=$?
grep -E "error|warning" "build/$1.log" | grep -v "deprecated" | head -40
echo "make $1 rc=$rc"
ls -l build/xmb_host build/xmb build/xmb.stripped 2>/dev/null
exit $rc
