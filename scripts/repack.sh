#!/usr/bin/env bash
# repack.sh <squashfs-root> <xImage> <ota_dir>  — root в WSL.
# Собирает содержимое .upt (ota_config.in + ota_v0/), ISO потом: python upt.py build <ota_dir> <out.upt>
#
# Формат ota_v0: образ режется на куски по 512K; кусок 0 назван md5 всего образа,
# кусок N — md5 куска N-1; ota_md5_<img>.<md5 образа> — md5 каждого куска по строке.
set -euo pipefail
root=$(realpath "$1"); ximage=$(realpath "$2"); out=$(realpath -m "$3")
rm -rf "$out"; mkdir -p "$out/ota_v0"; cd "$out/ota_v0"

pack() { # <name> <src>  -> печатает "<size> <md5>"
  local name=$1 src=$2 whole prev part m
  whole=$(md5sum "$src" | cut -d' ' -f1)
  split -d -a 4 -b 512k "$src" "$name."
  : > "ota_md5_$name.$whole"; prev=$whole
  for part in $(ls "$name".[0-9][0-9][0-9][0-9] | sort); do
    m=$(md5sum "$part" | cut -d' ' -f1)
    echo "$m" >> "ota_md5_$name.$whole"
    mv "$part" "$part.$prev"; prev=$m
  done
  echo "$(stat -c%s "$src") $whole"
}

sq=$(mktemp -u)
mksquashfs "$root" "$sq" -comp lzo -b 131072 -noappend -quiet >/dev/null
read -r ks km < <(pack xImage "$ximage")
read -r rs rm5 < <(pack rootfs.squashfs "$sq")
rm -f "$sq"
printf 'ota_version=0\n\nimg_type=kernel\nimg_name=xImage\nimg_size=%s\nimg_md5=%s\n\nimg_type=rootfs\nimg_name=rootfs.squashfs\nimg_size=%s\nimg_md5=%s\n\n' \
  "$ks" "$km" "$rs" "$rm5" > ota_update.in
echo > ota_v0.ok
echo current_version=0 > "$out/ota_config.in"
echo "ota -> $out (rootfs $rs bytes)"
