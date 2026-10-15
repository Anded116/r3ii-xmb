# autorun_check.sh — почему autorun.sh не запустил XMB: условие запуска с тем же DIR, что при загрузке
DIR=/data/mnt/sd_0/hiby_dev
X=$DIR/xmb
[ -f $X/xmb ] && echo "есть $X/xmb" || echo "нет $X/xmb"
[ ! -f $DIR/xmb_off ] && echo "xmb_off нет"
echo "fails: $(cat $DIR/xmb_fails 2>/dev/null || echo 0)"
mount | grep sd_0
grep -n "== xmb\|== end\|== adb" $DIR/autorun.log
