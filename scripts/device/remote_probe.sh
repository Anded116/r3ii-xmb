# remote_probe.sh [секунд] — сырые события пульта на проводе 3.5 (earpods_adc) и всех кнопок, состояние разъёма и
# новые строки журнала ядра — в hiby_dev/remote_probe.log. Запуск отвязанным, потом нажимать кнопки пульта.
N=${1:-60}
LOG=/data/mnt/sd_0/hiby_dev/remote_probe.log
echo "=== $(date +%T) $N с; разъём: headset $(cat /sys/class/switch/headset/state), balance $(cat /sys/class/switch/balance/state)" > $LOG
for e in /sys/class/input/event*; do echo "$(basename $e): $(cat $e/device/name)" >> $LOG; done
ls /sys/devices/platform | grep -i -E "earpod|adc" >> $LOG
for f in /sys/module/sa_earpods_adc/parameters/*; do [ -e "$f" ] && echo "$f = $(cat $f)" >> $LOG; done
dmesg > /tmp/rp0
for e in /dev/input/event*; do
	( timeout $N od -An -tx2 -w16 $e | sed "s|^|$(basename $e): |" >> $LOG ) &
done
i=0
while [ $i -lt $N ]; do
	sleep 1; i=$((i + 1))
	dmesg > /tmp/rp1
	diff /tmp/rp0 /tmp/rp1 | grep '^+[^+]' | grep -v 'nda=\|GTP\|DHCP\|wpa\|CFG80211\|MACEVENT' | cut -c2- >> $LOG
	mv /tmp/rp1 /tmp/rp0
	sync
done
echo "=== $(date +%T) конец; headset $(cat /sys/class/switch/headset/state)" >> $LOG
sync
