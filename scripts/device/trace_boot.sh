# trace_boot.sh [сек] — перезапустить стоковый плеер под strace с самого старта
S=${1:-90}
sh /tmp/stop_stock.sh
strace -f -tt -s 96 -e trace=open,openat,ioctl,write,mmap2 -o /tmp/boot.log /usr/bin/hiby_player > /dev/null 2>&1 &
echo "hiby_player under strace for ${S}s"
sleep $S
kill $(pidof strace) 2>/dev/null
sleep 1
echo "lines: $(wc -l < /tmp/boot.log)"
