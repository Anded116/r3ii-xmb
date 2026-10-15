# port_probe.sh — перебрать Output Port Switch 0..7 без воспроизведения и показать реакцию ядра
sh /tmp/stop_stock.sh
dmesg -c > /dev/null
for v in 0 1 2 3 4 5 6 7; do
	amixer -c 0 cset name='Output Port Switch' $v > /dev/null 2>&1
	sleep 0.5
	echo "== port $v -> get $(amixer -c 0 cget name='Output Port Switch' | grep ': values' | sed 's/.*=//')"
	dmesg -c | sed 's/^<[0-9]>\[ *[0-9.]*\] /   /'
done
