# bench_xmb.sh — CPU каждого стиля фона и проверка кнопки экрана («пред. трек» на корпусе = KEY_NEXTSONG 163 на event2)
sh /tmp/stop_stock.sh
killall run_xmb.sh xmb 2>/dev/null
chmod +x /tmp/xmb/xmb /tmp/inject
BL=/sys/class/backlight/backlight_pwm0/brightness
for w in 0 1 2 3; do
	XMB_WAVE=$w /tmp/xmb/xmb /tmp/xmb/fonts > /tmp/xmb_$w.log 2>&1 &
	sleep 3
	echo "wave $w: $(sh /tmp/cpu.sh xmb 6)  $(grep fps /tmp/xmb_$w.log | tail -1)"
	killall xmb; sleep 1
done
echo "== screen button"
XMB_WAVE=0 /tmp/xmb/xmb /tmp/xmb/fonts > /tmp/xmb_s.log 2>&1 &
sleep 3
echo "start: bl=$(cat $BL)"
/tmp/inject /dev/input/event2 163 1 163 0; sleep 1; echo "press1: bl=$(cat $BL)"
/tmp/inject /dev/input/event2 163 1 163 0; sleep 1; echo "press2: bl=$(cat $BL)"
/tmp/inject /dev/input/event2 163 1 163 0; sleep 1; echo "press3: bl=$(cat $BL)"
/tmp/inject /dev/input/event2 163 1 163 0; sleep 6; echo "press4: bl=$(cat $BL) $(grep fps /tmp/xmb_s.log | tail -1)"
killall xmb
