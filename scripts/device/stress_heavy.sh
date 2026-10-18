# stress_heavy.sh — плавность в самых тяжёлых условиях: FLAC 24/192, эквалайзер включён, тяжёлые фоны, все основные
# сценарии prof_xmb.sh; печатает строки fps и недогрузки звука. Настройки, последний трек и очередь — сохраняются и
# возвращаются, XMB перезапускается с ними.
# Переменные: TRACK=файл (по умолчанию — образ Crystal Castles 24/192), WAVES="5 3 7", SCEN="list grid player2 popen".
X=/data/mnt/sd_0/.xmb
TRACK=${TRACK:-"/data/mnt/sd_0/Crystal Castles [Vinyl]/Crystal Castles (2008)/Crystal Castles - Crystal Castles.flac"}
mkdir -p /tmp/heavy_save && cp $X/settings.txt $X/last.txt $X/queue.txt /tmp/heavy_save/ 2>/dev/null
restore() {
	sh /tmp/stop_xmb.sh > /dev/null
	cp /tmp/heavy_save/* $X/ 2>/dev/null
	rm -f $X/queue.txt.tmp
	setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
	sleep 3
}
trap 'restore; exit 1' HUP INT TERM
sh /tmp/stop_xmb.sh > /dev/null
sed -i 's/^eq_on=.*/eq_on=1/' $X/settings.txt
grep -q '^eq_on=' $X/settings.txt || echo eq_on=1 >> $X/settings.txt
printf 'path=%s\nstart=0.000\npos=600.0\nqi=0\n' "$TRACK" > $X/last.txt
rm -f $X/queue.txt
for w in ${WAVES:-5 3 7}; do
	for sc in ${SCEN:-list grid player2 popen}; do
		echo "=== фон $w, $sc"
		WAVE=$w MUSIC=1 sh /tmp/prof_xmb.sh $sc 2>&1 | grep -E "^fps"
		grep -E "недогрузка|USB-ЦАП|pcm_open" /tmp/prof.log
	done
done
trap - HUP INT TERM
restore
