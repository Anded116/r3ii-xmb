#!/usr/bin/env bash
# unpack.sh <iso_dir> <workdir>  — root в WSL. iso_dir: результат `upt.py extract`.
# Даёт <workdir>/squashfs-root (с исходными правами/владельцами) и <workdir>/xImage.
set -euo pipefail
iso=$(realpath "$1"); work=$2
rm -rf "$work"; mkdir -p "$work"
cd "$iso/ota_v0"
cat $(ls rootfs.squashfs.[0-9]* | sort) > "$work/rootfs.squashfs"
cat $(ls xImage.[0-9]* | sort) > "$work/xImage"
unsquashfs -q -d "$work/squashfs-root" "$work/rootfs.squashfs" >/dev/null
echo "unpacked -> $work/squashfs-root"
