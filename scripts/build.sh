#!/usr/bin/env bash
# build.sh <iso_dir стока> <out_ota_dir>  — root в WSL.
# Сток + overlay/ + правки ниже → ota_dir; ISO потом: python scripts/upt.py build <ota_dir> <out.upt>
set -euo pipefail
P=$(realpath "$(dirname "$0")/.."); W=/root/hiby
bash "$P/scripts/unpack.sh" "$1" $W/stock
rm -rf $W/mod; cp -a $W/stock $W/mod
R=$W/mod/squashfs-root

sed -i 's/{"usb_mode":0}/{"usb_mode":1}/' $R/usr/resource/set_functions.json
cp -r --no-preserve=ownership "$P/overlay/." $R/
chmod 755 $R/etc/init.d/S95sd_autorun $R/etc/init.d/S92_03_start_music_player $R/usr/bin/xmb_boot.sh
[ -d "$P/overlay/module_driver" ] && chmod 664 $R/module_driver/lcd_st77903_8bit.sh $R/module_driver/lcd_st77903_8bit_*.ko   # как у соседних модулей стока

diff -r --no-dereference $W/stock/squashfs-root $R || true
bash "$P/scripts/repack.sh" $R $W/mod/xImage "$2"
