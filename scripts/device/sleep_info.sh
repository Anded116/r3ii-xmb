# sleep_info.sh — что умеет сон: состояния, будильник RTC, какие устройства будят
echo "state: $(cat /sys/power/state)"
echo "wakealarm: $(cat /sys/class/rtc/rtc0/wakealarm 2>&1)"
which setsid nohup
for f in /sys/class/input/input*; do
	echo "$(cat $f/name 2>/dev/null): wakeup=$(cat $f/device/power/wakeup 2>/dev/null)"
done
for f in /sys/devices/platform/*/power/wakeup; do
	echo "$f: $(cat $f)"
done 2>/dev/null | grep -v ': $' | head -20
cat /sys/power/wakeup_count 2>&1
