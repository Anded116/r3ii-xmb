# trace_screen.sh [сек] — strace стокового hiby_player: что он делает при гашении/включении экрана
P=$(pidof hiby_player); S=${1:-40}
echo "hiby_player pid=$P, trace ${S}s"
strace -f -tt -s 80 -e trace=open,openat,ioctl,write -p $P -o /tmp/st.log &
SP=$!
sleep $S
kill $SP
sleep 1
echo "lines: $(wc -l < /tmp/st.log)"
