# sd_scan.sh — что на карте: смонтировать при необходимости, посчитать форматы и обложки
SD=/data/mnt/sd_0
if ! grep -q " $SD " /proc/mounts; then
	for l in /sys/kernel/config/usb_gadget/*/functions/mass_storage.*/lun.*; do echo > $l/file 2>/dev/null; done
	mkdir -p $SD; mount -t exfat /dev/mmcblk0p1 $SD && echo "mounted $SD"
fi
grep " $SD " /proc/mounts
echo "== extensions"
find $SD -type f 2>/dev/null | grep -v '/\.' | sed 's/.*\.//' | tr 'A-Z' 'a-z' | sort | uniq -c | sort -rn | head -15
echo "== cover files"
find $SD -type f \( -iname 'cover.*' -o -iname 'folder.*' -o -iname 'front.*' -o -iname '*.jpg' -o -iname '*.png' \) 2>/dev/null | grep -v '/\.' | head -20
echo "== top dirs"; ls $SD
