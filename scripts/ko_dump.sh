#!/bin/bash
# ko_dump.sh <модуль.ko> — секции, символы и дизассемблер модуля ядра (тулчейн musl в WSL)
T=/root/toolchains/mipsel-unknown-linux-musl/bin/mipsel-unknown-linux-musl
"$T-readelf" -S "$1"
"$T-readelf" -s "$1" | grep -v -E ' (FILE|SECTION) '
"$T-readelf" -r "$1" | head -200
"$T-objdump" -dr "$1"
