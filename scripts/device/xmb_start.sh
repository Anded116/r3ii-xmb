# xmb_start.sh <hiby_dev> — запуск XMB вместо стокового плеера при загрузке (вызывает autorun.sh).
# Установка — папка hiby_dev/xmb (scripts/deploy_xmb.ps1), на запуск копируется в /tmp: карта может уйти в режим
# USB-накопителя. Сток остаётся запасным: выход из XMB («Настройки → Стоковый плеер») — следующая загрузка в сток
# (xmb_skip_once); два падения подряд в первую минуту (xmb_fails) — XMB не стартует, пока файл не удалён.
# Убрать автозапуск — положить в hiby_dev файл xmb_off или удалить папку xmb.
# Прошивка с xmb_boot.sh запускает XMB сама из внутренней памяти — тогда здесь только установка туда (autorun.sh);
# отсюда XMB стартует, лишь если его там ещё нет (первая загрузка после прошивки).
DIR=${1:-/data/mnt/sd_0/hiby_dev}
X=$DIR/xmb
if pidof xmb > /dev/null; then
	echo "xmb: уже запущен"
	exit 0
fi
if [ -f /tmp/xmb_stock ]; then
	echo "xmb: эта загрузка — сток ($(cat /tmp/xmb_stock))"
	exit 0
fi
# с подключённым кабелем карта при загрузке на время уходит в USB-накопитель — ждём, пока вернётся (до 2 мин)
i=0
while [ ! -f $X/xmb ] && [ $i -lt 120 ]; do sleep 1; i=$((i+1)); done
echo "xmb: карта через $i с"
if [ ! -f $X/xmb ]; then
	echo "xmb: нет $X/xmb"
elif [ -f $DIR/xmb_off ]; then
	echo "xmb: выключен (xmb_off)"
elif [ -f $DIR/xmb_skip_once ]; then
	rm -f $DIR/xmb_skip_once
	echo "xmb: выход в сток, эта загрузка без XMB"
elif [ "$(cat $DIR/xmb_fails 2>/dev/null || echo 0)" -ge 2 ]; then
	echo "xmb: два падения подряд, запуск отключён (удалите xmb_fails)"
else
	echo "xmb: запуск"
	mkdir -p /tmp/xmb/fonts
	cp $X/xmb /tmp/xmb/xmb
	cp $X/fonts/* /tmp/xmb/fonts/
	cp $X/inject $X/*.sh /tmp/
	chmod +x /tmp/xmb/xmb /tmp/inject
	setsid sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
fi
