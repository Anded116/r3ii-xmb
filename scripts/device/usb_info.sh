# usb_info.sh — состояние USB: гаджеты configfs, UDC, питание, стоковые скрипты накопителя/ЦАП
G=/sys/kernel/config/usb_gadget
for g in $G/*; do
	echo "== $g UDC=[$(cat $g/UDC 2>/dev/null)]"
	ls $g/functions $g/configs/*/ 2>/dev/null | tr '\n' ' '; echo
	for l in $g/functions/mass_storage.*/lun.0; do [ -d $l ] && echo "  $l file=[$(cat $l/file)] ro=$(cat $l/ro) removable=$(cat $l/removable)"; done
done
for u in /sys/class/udc/*; do echo "udc $(basename $u): $(cat $u/state)"; done
for p in /sys/class/power_supply/*; do echo "psy $(basename $p): online=$(cat $p/online 2>/dev/null) type=$(cat $p/type 2>/dev/null)"; done
ls /sys/class/usb_gadget/ /sys/class/android_usb/ 2>&1 | head
ls /usr/bin/ | grep -i -E "usb|uac|mass|storage"
echo "== usb_dev_mass_storage.sh"; cat /usr/bin/usb_dev_mass_storage.sh 2>/dev/null
echo "== процессы"; ps | grep -i -E "usb|storage|sys_server" | grep -v grep
