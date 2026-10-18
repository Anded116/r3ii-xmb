# mem_play_test.sh [1|0] — «играть» после настоящего сна (XMB-0024): трек играет, пауза и подготовка ко сну
# (1 — как перед mem: трек закрыт; 0 — только PCM закрыт, трек открыт, как было раньше), mem с будильником RTC 15 с,
# пробуждение и «играть». Журнал — hiby_dev/mem_play.log; если плеер перезагрузился, в xmb.log будет «сброс: 0x…6».
# После — adbd заново и обычный XMB.
M=${1:-1}
LOG=/data/mnt/sd_0/hiby_dev/mem_play.log
rep() { s=""; i=0; while [ $i -lt $2 ]; do s="$s,$1"; i=$((i+1)); done; echo "$s"; }
sh /tmp/stop_xmb.sh > /dev/null
X=/data/mnt/sd_0/.xmb
mkdir -p /tmp/mem_save && cp $X/settings.txt $X/last.txt $X/queue.txt /tmp/mem_save/ 2>/dev/null
echo "=== $(date +%T) режим $M" > $LOG
sync
cd /tmp/xmb
XMB_STEP=100 XMB_SCRIPT="wait$(rep wait 20),play$(rep wait 50),sleep:$M$(rep wait 150),wake$(rep wait 10),play$(rep wait 100)" \
	./xmb /tmp/xmb/fonts >> $LOG 2>&1 &
P=$!
sleep 25
echo "$(date +%T) засыпаю" >> $LOG
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +15 > /sys/class/rtc/rtc0/wakealarm
sync
echo mem > /sys/power/state
echo "$(date +%T) проснулся (rc=$?)" >> $LOG
wait $P
echo "$(date +%T) xmb вышел: $?" >> $LOG
cp /tmp/mem_save/* $X/ 2>/dev/null
sync
setsid sh /tmp/adb_restart.sh > /dev/null 2>&1 < /dev/null &
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 3
