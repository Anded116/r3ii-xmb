# stop_xmb.sh — остановить прототип без перезагрузки (скрипт запуска убить раньше, иначе он ребутнёт)
killall run_xmb.sh 2>/dev/null
for p in $(ps | grep 'run_xmb.sh' | grep -v grep | awk '{print $1}'); do kill $p; done
killall xmb 2>/dev/null
sleep 0.5
ps | grep -E 'xmb' | grep -v grep
for d in axp_battery battery; do echo "$d: $(cat /sys/class/power_supply/$d/capacity) $(cat /sys/class/power_supply/$d/status)"; done
