# cpu.sh <имя процесса> [сек] — доля CPU процесса за интервал
P=$(pidof $1); S=${2:-5}
t1=$(awk '{print $14+$15}' /proc/$P/stat); a1=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
sleep $S
t2=$(awk '{print $14+$15}' /proc/$P/stat); a2=$(awk '/^cpu /{print $2+$3+$4+$5+$6+$7+$8}' /proc/stat)
echo "$1 cpu: $(( (t2-t1)*100/(a2-a1) ))%"
