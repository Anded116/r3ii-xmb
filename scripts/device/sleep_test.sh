# sleep_test.sh — проба глубокого сна (suspend to RAM): будильник RTC через 45 с как страховка.
# Лог на карте: hiby_dev/sleep_test.log — по длительности сна видно, разбудила кнопка (< 45 с) или будильник.
LOG=/data/mnt/sd_0/hiby_dev/sleep_test.log
sh /tmp/stop_stock.sh >/dev/null 2>&1
sleep 1
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +45 > /sys/class/rtc/rtc0/wakealarm
t0=$(date +%s)
echo "$(date +%T) засыпаю, будильник: $(cat /sys/class/rtc/rtc0/wakealarm)" > $LOG
sync
echo mem > /sys/power/state
rc=$?
t1=$(date +%s)
echo "$(date +%T) проснулся (rc=$rc) через $((t1 - t0)) с" >> $LOG
dmesg | grep -i -E 'suspend|resume|wake|PM:' | tail -15 >> $LOG
sync
