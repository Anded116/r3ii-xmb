# usb_storage_test.sh [сек] — проба режима накопителя: XMB остановлен, карта отмонтирована и отдана компьютеру
# стоковым гаджетом android0, через N секунд (60) всё возвращается: карта — плееру, контроллер — adb, XMB запущен.
# Журнал — /tmp/usbtest.log (карта в это время у компьютера). Запускать отвязанным (setsid), adb пропадёт.
N=${1:-60}
L=/tmp/usbtest.log
G=/sys/kernel/config/usb_gadget
MNT=/usr/data/mnt/sd_0
LUN=$G/android0/functions/mass_storage.0/lun.0/file
UDC=$(ls /sys/class/udc | head -n1)
exec > $L 2>&1
echo "== $(date) старт"
sh /tmp/stop_xmb.sh
sync
umount $MNT || { sleep 1; umount $MNT; } || umount -l $MNT
mount | grep sd_0 && echo "карта всё ещё смонтирована"
echo "" > $G/adb_demo/UDC
echo "" > $G/android0/UDC 2>/dev/null
echo /dev/mmcblk0p1 > $LUN
echo $UDC > $G/android0/UDC
sleep 2
echo "udc: $(cat /sys/class/udc/$UDC/state) lun: $(cat $LUN)"
i=0
while [ $i -lt $N ]; do
	sleep 1; i=$((i+1))
	[ -z "$(cat $LUN)" ] && { echo "компьютер извлёк диск через $i с"; break; }
done
echo "== возврат $(date)"
echo "" > $LUN
echo "" > $G/android0/UDC
mount -t exfat -o rw,relatime,fmask=0022,dmask=0022,iocharset=utf8,namecase=0 /dev/mmcblk0p1 $MNT
mount | grep sd_0 || echo "карта не смонтировалась"
echo $UDC > $G/adb_demo/UDC
sleep 1
echo "udc: $(cat /sys/class/udc/$UDC/state)"
cp $L $MNT/hiby_dev/usbtest.log
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 3
echo "== $(date) готово"
cp $L $MNT/hiby_dev/usbtest.log
