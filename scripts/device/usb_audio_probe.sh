# usb_audio_probe.sh [секунд] — что появляется, когда в разъём вставлен USB-ЦАП (адаптер USB-C → 3.5). Ждёт (до 30 мин),
# пока вынут кабель от компьютера, потом раз в секунду — устройства USB, звуковые устройства /dev/snd и новые строки
# журнала ядра в hiby_dev/usb_audio.log (sync каждый раз). Запуск отвязанным; дальше — вынуть кабель, вставить
# адаптер, подождать, вынуть, снова кабель.
N=${1:-90}
LOG=/data/mnt/sd_0/hiby_dev/usb_audio.log
echo "=== $(date +%T) ждём, пока вынут кабель" > $LOG
w=0
while [ "$(cat /sys/class/power_supply/usb/online 2>/dev/null)" = 1 ] && [ $w -lt 1800 ]; do sleep 1; w=$((w + 1)); done
echo "=== $(date +%T) кабель вынут — пишем $N с" >> $LOG
dmesg > /tmp/uap_dmesg0
prev=""
i=0
while [ $i -lt $N ]; do
	i=$((i + 1))
	cur="snd: $(ls /dev/snd | tr '\n' ' ')| usb: $(ls /sys/bus/usb/devices | tr '\n' ' ')| udc: $(cat /sys/class/udc/*/state 2>/dev/null)| vbus: $(cat /sys/class/power_supply/usb/online 2>/dev/null)"
	if [ "$cur" != "$prev" ]; then
		echo "$(date +%T) $cur" >> $LOG
		for d in /sys/bus/usb/devices/[0-9]*-[0-9]*; do
			[ -e $d/idVendor ] && echo "   $d: $(cat $d/idVendor):$(cat $d/idProduct) $(cat $d/manufacturer 2>/dev/null) $(cat $d/product 2>/dev/null) speed=$(cat $d/speed 2>/dev/null)" >> $LOG
		done
		for c in /proc/asound/card*/stream* /sys/class/sound/card*/id; do
			[ -e $c ] && echo "   $c: $(head -3 $c | tr '\n' ' ')" >> $LOG
		done
		prev=$cur
	fi
	dmesg > /tmp/uap_dmesg1
	diff /tmp/uap_dmesg0 /tmp/uap_dmesg1 | grep '^+[^+]' | grep -v 'nda=\|GTP\|DHCP\|wpa\|CFG80211\|MACEVENT' | cut -c2- | head -40 >> $LOG
	mv /tmp/uap_dmesg1 /tmp/uap_dmesg0
	sync
	sleep 1
done
echo "$(date +%T) конец" >> $LOG
sync
