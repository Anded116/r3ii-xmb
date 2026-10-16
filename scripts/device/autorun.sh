DIR=${1:-/data/mnt/sd_0/hiby_dev}
# запуск XMB (xmb_start.sh) — копией в /tmp: с подключённым кабелем карта на время уходит в USB-накопитель
cp $DIR/xmb/xmb_start.sh /tmp/ 2>/dev/null

echo "== info $(date)"
uname -a
cat /etc/ota_info
head -12 /proc/cpuinfo
free
df -h
mount
echo "== input"
cat /proc/bus/input/devices
ls -l /dev/input
echo "== fb"
for f in virtual_size bits_per_pixel stride modes name rotate; do echo "$f: $(cat /sys/class/graphics/fb0/$f 2>/dev/null)"; done
fbset 2>/dev/null
echo "== usb"
ls /sys/class/udc; cat /sys/class/udc/*/state
ls /sys/kernel/config/usb_gadget 2>/dev/null
echo "== net"
ifconfig -a
echo "== ps"
ps

if [ -f $DIR/capture ]; then
	echo "== input capture 30s $(date)"
	for d in /dev/input/event*; do
		cat $d > $DIR/ev_$(basename $d).bin &
		pids="$pids $!"
	done
	sleep 30
	kill $pids 2>/dev/null
	echo "== capture done $(date)"
	ls -l $DIR
fi

if [ -f $DIR/adb ] || [ -f $DIR/adb_keep ]; then
	[ -f $DIR/adb ] && mv $DIR/adb $DIR/adb.done
	echo "== adb"
	sync
	G=/sys/kernel/config/usb_gadget
	[ -d $G ] || mount -t configfs none /sys/kernel/config
	for g in $G/*; do [ -f $g/UDC ] && echo "unbind $g" && echo "" > $g/UDC; done
	mkdir -p $G/adb_demo && cd $G/adb_demo
	echo 0x18d1 > idVendor
	echo 0xd002 > idProduct
	mkdir -p strings/0x409 configs/c.1/strings/0x409 functions/ffs.adb
	echo HiBy > strings/0x409/manufacturer
	echo R3II_2025 > strings/0x409/product
	echo r3ii2025 > strings/0x409/serialnumber
	echo adb > configs/c.1/strings/0x409/configuration
	echo 120 > configs/c.1/MaxPower
	[ -e configs/c.1/ffs.adb ] || ln -s functions/ffs.adb configs/c.1/
	mkdir -p /dev/usb-ffs/adb
	mount -t functionfs adb /dev/usb-ffs/adb
	killall adbd 2>/dev/null
	adbd &
	sleep 2
	ls /sys/class/udc > UDC
	echo "UDC=$(cat UDC) state=$(cat /sys/class/udc/*/state)"
	ps | grep -v grep | grep adbd
fi

# прошивка с xmb_boot.sh запускает XMB из внутренней памяти — установить или обновить её с карты
# (только изменившиеся файлы: флеш не переписывать на каждой загрузке)
if [ -x /usr/bin/xmb_boot.sh ] && [ -f $DIR/xmb/xmb ]; then
	mkdir -p /usr/data/xmb/fonts
	n=0
	for f in $DIR/xmb/xmb $DIR/xmb/inject $DIR/xmb/*.sh $DIR/xmb/fonts/*; do
		t=/usr/data/xmb/${f#$DIR/xmb/}
		cmp -s $f $t || { cp $f $t.new && mv $t.new $t && n=$((n+1)); }
	done
	chmod +x /usr/data/xmb/xmb /usr/data/xmb/inject
	sync
	echo "== xmb: во внутренней памяти обновлено файлов: $n"
fi

# XMB вместо стокового плеера (scripts/device/xmb_start.sh в папке установки)
[ -f /tmp/xmb_start.sh ] && setsid sh /tmp/xmb_start.sh $DIR > /tmp/xmb_start.log 2>&1 < /dev/null &
echo "== end $(date)"
sync
