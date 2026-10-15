# wakeup_info.sh — какие устройства могут будить из сна: power/wakeup у кнопок (event0 gpio, event2 АЦП) и родителей
for e in event0 event2 event3; do
	d=$(readlink -f /sys/class/input/$e/device)
	echo "== $e: $(cat /sys/class/input/$e/device/name)  $d"
	p=$d
	while [ "$p" != "/" ] && [ "$p" != "/sys" ]; do
		[ -f $p/power/wakeup ] && echo "  $p/power/wakeup = $(cat $p/power/wakeup)"
		p=$(dirname $p)
	done
done
echo "== wakeup_sources"
cat /sys/kernel/debug/wakeup_sources 2>/dev/null | head -30
ls /sys/class/wakeup 2>/dev/null
grep -i -E "adc|sadc|key|wake" /proc/interrupts
