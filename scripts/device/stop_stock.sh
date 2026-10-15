# stop_stock.sh — остановить стоковый плеер, не оставив экран выключенным.
# Если сток погасил экран и завершится в этом состоянии, панель потом не будится (pan уходит в таймаут),
# поэтому сначала даём ему самому включить экран нажатием колеса (KEY_POWER).
BL=/sys/class/backlight/backlight_pwm0/brightness
if pidof hiby_player >/dev/null && [ "$(cat $BL)" = "0" ]; then
	echo "stock screen is off, waking it"
	/tmp/inject /dev/input/event0 116 1 116 0
	i=0
	while [ "$(cat $BL)" = "0" ] && [ $i -lt 30 ]; do sleep 0.1; i=$((i+1)); done
	sleep 0.5
fi
killall hiby_player.sh 2>/dev/null
killall hiby_player 2>/dev/null
sleep 1
