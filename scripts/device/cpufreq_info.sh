# cpufreq_info.sh — частота процессора, регулятор, режимы простоя
D=/sys/devices/system/cpu/cpu0/cpufreq
ls $D 2>&1
for f in scaling_governor scaling_available_governors scaling_cur_freq scaling_min_freq scaling_max_freq scaling_available_frequencies cpuinfo_cur_freq stats/time_in_state; do
	[ -e $D/$f ] && echo "$f: $(cat $D/$f | tr '\n' ' ')"
done
ls /sys/devices/system/cpu/cpu0/cpuidle 2>&1 | head
ls /sys/devices/system/cpu/ 2>&1
grep -iE "system type|cpu model|BogoMIPS" /proc/cpuinfo
ls /sys/kernel/debug 2>&1 | head -3
cat /proc/clocks 2>/dev/null | grep -i cpu
