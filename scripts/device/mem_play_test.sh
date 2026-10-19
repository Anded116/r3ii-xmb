# mem_play_test.sh [кнопки] [пауза] — нажатия после настоящего сна (XMB-0024, XMB-0061). XMB: трек играет, затем
# сценарий «mem» — тот же deep_sleep, что от колеса (пауза, трек закрыт, Wi-Fi выключен, mem; после — Wi-Fi и adbd
# заново), на кабеле — с XMB_FORCE_MEM; будит будильник RTC через 10 с. После пробуждения через «пауза» секунд
# (по умолчанию 1) — кнопки корпуса через inject: «next play» по умолчанию («след. трек», затем «играть»), ещё «wheel».
# Журналы на карте: hiby_dev/mem_play.log (шаги и XMB), bb.log (чёрный ящик), mem_kmsg.log (ядро), sync каждые
# 0,2 с. Перезагрузился плеер — в xmb.log «сброс: 0x…6», чёрный ящик — в bb.log.prev. После — обычный XMB.
KEYS=${1:-next play}
GAP=${2:-1}
D=/data/mnt/sd_0/hiby_dev
LOG=$D/mem_play.log
rep() { s=""; i=0; while [ $i -lt $2 ]; do s="$s,$1"; i=$((i+1)); done; echo "$s"; }
step() { echo "$(date +%T) $*" >> $LOG; sync; }
sh /tmp/stop_xmb.sh > /dev/null
X=/data/mnt/sd_0/.xmb
mkdir -p /tmp/mem_save && cp $X/settings.txt $X/last.txt $X/queue.txt /tmp/mem_save/ 2>/dev/null
echo "=== $(date +%T) кнопки: $KEYS, через $GAP с" > $LOG
cat /proc/kmsg > $D/mem_kmsg.log &
K=$!
( while :; do sync; sleep 0.2; done ) &
S=$!
echo 0 > /sys/class/rtc/rtc0/wakealarm
cd /tmp/xmb
XMB_FORCE_MEM=1 XMB_LOG=$LOG XMB_STEP=100 XMB_SCRIPT="wait$(rep wait 20),play$(rep wait 50),mem$(rep wait 600)" \
	./xmb /tmp/xmb/fonts > /dev/null 2>&1 &
P=$!
sleep 4
echo +25 > /sys/class/rtc/rtc0/wakealarm   # сон — через ~8–15 с после старта сценария (радио гасится до ~5 с)
step "будильник: $(cat /sys/class/rtc/rtc0/wakealarm)"
until grep -q "^resume" $LOG; do sleep 0.1; done
step проснулся
sleep $GAP
for k in $KEYS; do
	case $k in
	next) /tmp/inject /dev/input/event0 165 1 165 0 ;;
	play) /tmp/inject /dev/input/event2 164 1 164 0 ;;
	wheel) /tmp/inject /dev/input/event0 116 1 116 0 ;;
	esac
	step "нажато: $k"
	sleep 2
done
sleep 8
step "жив, останавливаю"
kill $P $K $S
wait $P 2>/dev/null
cp /tmp/mem_save/* $X/ 2>/dev/null
sync
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 3
