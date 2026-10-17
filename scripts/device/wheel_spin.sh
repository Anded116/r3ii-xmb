# wheel_spin.sh <щелчков> [мс] — быстро крутить колесо вниз и обратно вверх (event3: 105 — вниз, 106 — вверх)
N=${1:-125}
D=$(( ${2:-30} * 1000 ))
I=/tmp/inject
for code in 105 106; do
	i=0
	while [ $i -lt $N ]; do
		$I /dev/input/event3 $code 1 $code 0
		usleep $D
		i=$((i + 1))
	done
	sleep 1
done
