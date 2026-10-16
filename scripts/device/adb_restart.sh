# adb_restart.sh — после глубокого сна: USB пересобирается, и adbd перестаёт открывать оболочку. Отвязать гаджет
# adb_demo, перезапустить adbd на той же functionfs и привязать снова. Без отладочного гаджета — ничего.
G=/sys/kernel/config/usb_gadget/adb_demo
[ -d $G ] && [ -n "$(cat $G/UDC 2>/dev/null)" ] || exit 0
echo "" > $G/UDC
killall adbd 2>/dev/null
sleep 1
adbd > /dev/null 2>&1 < /dev/null &
sleep 1
ls /sys/class/udc > $G/UDC
