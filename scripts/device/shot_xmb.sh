# shot_xmb.sh "<сценарий>" <секунд> [<кадров> <мс>] — запустить XMB со сценарием (шаг 80 мс, громкость 0), через
# <секунд> снять экран в /tmp/fb.raw (обе страницы fb0) или серию /tmp/fb_N.raw через <мс>; затем вернуть обычный XMB.
# Сценарий — как XMB_SCRIPT; "w20" — 20 раз wait.
sh /tmp/stop_xmb.sh > /dev/null
S=""
for t in $(echo "$1" | tr ',' ' '); do
	case $t in
	w[0-9]*) n=${t#w}; i=0; while [ $i -lt $n ]; do S="$S,wait"; i=$((i+1)); done ;;
	*) S="$S,$t" ;;
	esac
done
cd /tmp/xmb
XMB_STEP=80 XMB_SCRIPT="${S#,}" ./xmb /tmp/xmb/fonts > /tmp/shot.log 2>&1 &
P=$!
sleep $2
if [ -n "$3" ]; then
	rm -f /tmp/fb_*.raw
	i=0
	while [ $i -lt $3 ]; do
		cat /dev/fb0 > /tmp/fb_$i.raw
		usleep $(($4 * 1000))
		i=$((i+1))
	done
else
	cat /dev/fb0 > /tmp/fb.raw
fi
kill $P 2>/dev/null
wait $P 2>/dev/null
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 2
