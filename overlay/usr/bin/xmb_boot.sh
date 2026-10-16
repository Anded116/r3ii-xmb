#!/bin/sh
# xmb_boot.sh — плеер при загрузке (S92_03_start_music_player): XMB из внутренней памяти /usr/data/xmb вместо
# стокового, без стока на экране. Сток — запасной: XMB не установлен, xmb_off, xmb_skip_once (из XMB выбран
# «Стоковый плеер»), два падения подряд в первую минуту (xmb_fails). Устанавливает и обновляет /usr/data/xmb
# autorun.sh с карты (hiby_dev/xmb) или deploy_xmb.ps1.
X=/usr/data/xmb
stock() {
	echo "$1" > /tmp/xmb_stock   # xmb_start.sh с карты в эту загрузку XMB не запускает
	exec /usr/bin/hiby_player.sh
}
[ -x $X/xmb ] || stock "не установлен"
[ -f $X/xmb_off ] && stock "xmb_off"
if [ -f $X/xmb_skip_once ]; then
	rm -f $X/xmb_skip_once
	sync
	stock "выход в сток"
fi
[ "$(cat $X/xmb_fails 2>/dev/null || echo 0)" -ge 2 ] && stock "два падения подряд"

# гаджет USB-накопителя — как его собирает сток, без карты (её отдаёт XMB по запросу)
[ -d /sys/kernel/config/usb_gadget/android0/functions/mass_storage.0 ] ||
	/usr/bin/usb_dev_mass_storage.sh start /none > /dev/null 2>&1

# карту монтирует сток (sys_server по его запросу) — без него монтируем сами
i=0
while [ ! -b /dev/mmcblk0p1 ] && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
if ! grep -q " /usr/data/mnt/sd_0 " /proc/mounts; then
	mount -t exfat -o relatime,fmask=0022,dmask=0022,iocharset=utf8,namecase=0 /dev/mmcblk0p1 /usr/data/mnt/sd_0 ||
		mount -t vfat,exfat /dev/mmcblk0p1 /usr/data/mnt/sd_0
fi

mkdir -p /tmp/xmb/fonts
cp $X/xmb /tmp/xmb/xmb
cp $X/fonts/* /tmp/xmb/fonts/
cp $X/*.sh $X/inject /tmp/ 2>/dev/null
chmod +x /tmp/xmb/xmb /tmp/inject
exec sh /tmp/run_xmb.sh
