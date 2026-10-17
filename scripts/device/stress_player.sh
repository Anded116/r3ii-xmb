# stress_player.sh <шагов> — нагрузка на полноэкранный плеер через inject: след. трек, перелистывание альбомов,
# перемотка тапом, закрыть и открыть плеер. Каждый шаг — в hiby_dev/stress.log (переживает перезагрузку).
# Громкость выставлять до запуска (тесты — на 0).
N=${1:-100}
LOG=/data/mnt/sd_0/hiby_dev/stress.log
I=/tmp/inject
echo "=== $(date) старт, шагов $N" >> $LOG
$I /dev/input/event1 drag 160 455 160 455 60   # открыть плеер с мини-плеера
sleep 2
i=0
while [ $i -lt $N ]; do
	r=$((RANDOM % 6))
	case $r in
	0|1) a="след. трек"; $I /dev/input/event0 165 1 165 0 ;;
	2) a="альбом вперёд"; $I /dev/input/event1 drag 250 150 60 150 250 ;;
	3) a="альбом назад"; $I /dev/input/event1 drag 60 150 250 150 250 ;;
	4) x=$((30 + RANDOM % 260)); a="перемотка $x"; $I /dev/input/event1 drag $x 362 $x 362 60 ;;
	5) a="закрыть и открыть"; $I /dev/input/event1 drag 160 200 160 420 250; sleep 1; $I /dev/input/event1 drag 160 455 160 455 60 ;;
	esac
	echo "$(date +%T) $i $a" >> $LOG
	[ $((i % 10)) -eq 0 ] && sync
	usleep $((300000 + RANDOM % 1500000))
	i=$((i + 1))
done
echo "=== $(date) конец" >> $LOG
sync
