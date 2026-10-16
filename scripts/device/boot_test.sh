# boot_test.sh — проверить xmb_boot.sh (запуск XMB из внутренней памяти) без перепрошивки: XMB ставится в
# /usr/data/xmb с карты, обычный XMB останавливается, карта отмонтируется, xmb_boot.sh запускается как из S92.
# Сборка гаджета накопителя забирает контроллер у adb — через 5 с adb возвращается. Журнал — /tmp/boot_test.log
exec > /tmp/boot_test.log 2>&1
D=/data/mnt/sd_0/hiby_dev
mkdir -p /usr/data/xmb/fonts
cp $D/xmb/xmb $D/xmb/inject $D/xmb/*.sh /usr/data/xmb/
cp $D/xmb/fonts/* /usr/data/xmb/fonts/
chmod +x /usr/data/xmb/xmb /usr/data/xmb/inject
sync
sh /tmp/stop_xmb.sh
umount -l /usr/data/mnt/sd_0 && echo "карта отмонтирована"
t0=$(date +%s)
setsid sh /tmp/xmb_boot.sh > /tmp/xmb_boot.out 2>&1 < /dev/null &
i=0
while ! pidof xmb > /dev/null && [ $i -lt 100 ]; do sleep 0.1; i=$((i+1)); done
echo "xmb через $((i * 100)) мс, pid $(pidof xmb)"
grep " /usr/data/mnt/sd_0 " /proc/mounts && echo "карта смонтирована"
G=/sys/kernel/config/usb_gadget
echo "android0: UDC=$(cat $G/android0/UDC) функции: $(ls $G/android0/configs/c.1/) lun=$(cat $G/android0/functions/mass_storage.0/lun.0/file)"
sleep 5
for g in $G/*; do [ -f $g/UDC ] && echo "" > $g/UDC; done
ls /sys/class/udc > $G/adb_demo/UDC
echo "adb возвращён: $(cat $G/adb_demo/UDC)"
