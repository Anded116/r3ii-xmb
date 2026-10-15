# run_xmb.sh — XMB вместо стокового плеера; выход («Настройки → Стоковый плеер») или падение → reboot.
# Выход — следующая загрузка в сток (xmb_skip_once для autorun.sh). Падение в первую минуту считается в xmb_fails:
# после двух подряд autorun.sh XMB не запускает. Проработал дольше минуты — счётчик сброшен.
# Журнал — на карте (hiby_dev/xmb.log, прошлый запуск — xmb.log.prev): /tmp при перезагрузке стирается.
# Нет строки «xmb exit» в конце — перезагрузилось ядро, а не XMB. Код 3 — перезапуск XMB без перезагрузки.
D=/data/mnt/sd_0/hiby_dev
LOG=$D/xmb.log
mv -f $LOG $LOG.prev 2>/dev/null
echo "=== $(date) запуск" > $LOG
sh /tmp/stop_stock.sh >> $LOG 2>&1
chmod +x /tmp/xmb/xmb
while :; do
	t0=$(date +%s)
	XMB_LOG=$LOG /tmp/xmb/xmb /tmp/xmb/fonts > /dev/null 2>&1   # журнал XMB открывает сам: «>> $LOG» держал бы карту
	rc=$?
	dt=$(( $(date +%s) - t0 ))
	[ $rc -eq 3 ] || break   # 3 — после режима USB-накопителя: перезапуск, библиотека перечитывается
	echo "=== $(date) перезапуск после USB" >> $LOG
done
if [ $rc -eq 0 ]; then
	rm -f $D/xmb_fails
	touch $D/xmb_skip_once
elif [ $dt -lt 60 ]; then
	echo $(( $(cat $D/xmb_fails 2>/dev/null || echo 0) + 1 )) > $D/xmb_fails
else
	rm -f $D/xmb_fails
fi
echo "xmb exit $rc after ${dt}s, rebooting" >> $LOG
dmesg | tail -40 >> $LOG
sync
reboot
