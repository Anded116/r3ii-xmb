# Монтирует SD (ro) в /tmp/sd, если гаджет mass_storage держит её — показать кто
for g in /sys/kernel/config/usb_gadget/*; do
	echo "$g UDC=[$(cat $g/UDC)]"
	for l in $g/functions/mass_storage.*/lun.*; do [ -f $l/file ] && echo "  $l file=[$(cat $l/file)]"; done
done
mkdir -p /tmp/sd
for l in /sys/kernel/config/usb_gadget/*/functions/mass_storage.*/lun.*; do
	[ -n "$(cat $l/file 2>/dev/null)" ] && { echo 1 > $l/forced_eject 2>/dev/null || echo > $l/file; echo "released $l"; }
done
grep -q /tmp/sd /proc/mounts || mount -t exfat -o ro /dev/mmcblk0p1 /tmp/sd || mount -o ro /dev/mmcblk0p1 /tmp/sd
grep /tmp/sd /proc/mounts && ls -la /tmp/sd/hiby_dev
