# threads_cpu.sh [секунд] — загрузка CPU по потокам xmb за интервал (utime+stime, тики по 10 мс)
S=${1:-5}
P=$(pidof xmb)
[ -z "$P" ] && { echo "xmb не запущен"; exit 1; }
snap() { for t in /proc/$P/task/*; do echo "${t##*/} $(cut -d' ' -f14,15 $t/stat)"; done; }
A=$(snap)
sleep $S
snap | while read tid u s; do
	old=$(echo "$A" | grep "^$tid " | cut -d' ' -f2,3)
	ou=${old% *}; os=${old#* }
	d=$(( (u + s - ${ou:-0} - ${os:-0}) ))
	pol=$(cut -d' ' -f41 /proc/$P/task/$tid/stat)
	echo "поток $tid (policy $pol): $(( d * 100 / (S * 100) ))% CPU"
done
