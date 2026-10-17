# run_xmb.sh — XMB вместо стокового плеера; выход («Настройки → Стоковый плеер») или падение → reboot.
# Выход — следующая загрузка в сток (xmb_skip_once). Падение в первую минуту считается в xmb_fails: после двух
# подряд при загрузке XMB не запускается. Проработал дольше минуты — счётчик сброшен. Флаги — во внутренней памяти
# (/usr/data/xmb, прошивка с xmb_boot.sh), без неё — на карте (hiby_dev, для autorun.sh).
# Журнал — на карте (hiby_dev/xmb.log, прошлый запуск — xmb.log.prev), без карты — /tmp/xmb.log.
# Нет строки «xmb exit» в конце — перезагрузилось ядро, а не XMB. Код 3 — перезапуск XMB без перезагрузки, 4 — выключение.
D=/data/mnt/sd_0/hiby_dev
F=$D
[ -x /usr/bin/xmb_boot.sh ] && F=/usr/data/xmb
LOG=$D/xmb.log
[ -d $D ] || LOG=/tmp/xmb.log
mv -f $LOG $LOG.prev 2>/dev/null
echo "=== $(date) запуск" > $LOG
sh /tmp/stop_stock.sh >> $LOG 2>&1
chmod +x /tmp/xmb/xmb
warm=
while :; do
	t0=$(date +%s)
	# журнал XMB открывает сам: «>> $LOG» держал бы карту; XMB_WARM — перезапуск: панель уже работает, не гасить
	XMB_WARM=$warm XMB_LOG=$LOG /tmp/xmb/xmb /tmp/xmb/fonts > /dev/null 2>&1
	rc=$?
	dt=$(( $(date +%s) - t0 ))
	[ $rc -eq 3 ] || break   # 3 — после режима USB-накопителя или удаления: перезапуск, библиотека перечитывается
	echo "=== $(date) перезапуск" >> $LOG
	warm=1
done
if [ $rc -eq 4 ]; then   # батарея села: выключить, а не перезагрузить
	echo "xmb exit 4: батарея села, выключение" >> $LOG
	rm -f $F/xmb_fails
	sync
	poweroff
	exit 0
fi
if [ $rc -eq 0 ]; then
	rm -f $F/xmb_fails
	touch $F/xmb_skip_once
elif [ $dt -lt 60 ]; then
	echo $(( $(cat $F/xmb_fails 2>/dev/null || echo 0) + 1 )) > $F/xmb_fails
else
	rm -f $F/xmb_fails
fi
echo "xmb exit $rc after ${dt}s, rebooting" >> $LOG
dmesg | tail -40 >> $LOG
sync
reboot
