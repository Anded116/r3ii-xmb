# Останавливает стоковый плеер (без его авто-reboot), включает подсветку, запускает /tmp/touchtest.
# После выхода (кнопка Power) — reboot, чтобы вернуть сток.
killall hiby_player.sh 2>/dev/null
killall hiby_player 2>/dev/null
sleep 1
for b in /sys/class/backlight/*; do
	echo "$b: $(cat $b/brightness)/$(cat $b/max_brightness)"
	cat $b/max_brightness > $b/brightness
	echo 0 > $b/bl_power 2>/dev/null
done
echo 0 > /sys/class/graphics/fb0/blank 2>/dev/null
chmod +x /tmp/touchtest
/tmp/touchtest
echo "touchtest exit $?, rebooting"
sync
reboot
