# wake_probe.sh [секунд] [повторов] — кто будит плеер из сна: XMB остановлен, радио выключено как перед сном XMB
# (wifi_off.sh, bt_suspend), будильник RTC через N с (120); после пробуждения — сколько спал и какие прерывания
# прибавились. Журнал — hiby_dev/wake_probe.log. После — adbd и обычный XMB.
N=${1:-120}
R=${2:-3}
LOG=/data/mnt/sd_0/hiby_dev/wake_probe.log
sh /tmp/stop_xmb.sh > /dev/null
echo "=== $(date +%T) будильник $N с, повторов $R" > $LOG
sh /usr/bin/wifi_off.sh > /dev/null 2>&1
sh /usr/bin/bt_suspend > /dev/null 2>&1
echo "rfkill: $(cat /sys/class/rfkill/rfkill*/state | tr '\n' ' ')" >> $LOG
sync
i=0
while [ $i -lt $R ]; do
	i=$((i + 1))
	cat /proc/interrupts > /tmp/irq_a
	echo 0 > /sys/class/rtc/rtc0/wakealarm
	echo +$N > /sys/class/rtc/rtc0/wakealarm
	t0=$(cut -d' ' -f1 /proc/uptime)
	b0=$(date +%s)
	sync
	echo mem > /sys/power/state
	b1=$(date +%s)
	cat /proc/interrupts > /tmp/irq_b
	echo "--- круг $i: спал $((b1 - b0)) с" >> $LOG
	awk 'NR==FNR { a[$1] = $2; next } $1 in a && $2 != a[$1] { $2 = $2 - a[$1]; print "  +" $2, $0 }' /tmp/irq_a /tmp/irq_b \
		| cut -c1-90 >> $LOG
	sync
	sleep 3
done
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo "$(date +%T) конец" >> $LOG
sync
setsid sh /tmp/adb_restart.sh > /dev/null 2>&1 < /dev/null &
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 3
