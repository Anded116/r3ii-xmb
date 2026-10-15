# prof_xmb.sh [list|grid|back|artist|cats|opts|opts2|player|player2|popen|pidle|play] — замер плавности и нагрузки XMB по сценарию (громкость 0).
# list — прокрутка «Всех треков»; grid — сетка альбомов; back — вход в альбом и возврат в сетку (обложки);
# artist — исполнитель → сетка его альбомов → альбом и обратно, по шести исполнителям;
# cats — переключение категорий; opts — прокрутка меню выбора (тема, с предпросмотром); opts2 — меню без
# тяжёлого предпросмотра (яркость); player — смена альбомов в плеере; player2 — туда и обратно; popen — раскрыть и свернуть плеер; pidle — плеер в покое;
# play — плеер играет, затем экран гаснет.
# Переменные: WAVE=номер стиля фона, MUSIC=1 — через 3 с после старта включить музыку, PLAY=1 — нажать «играть/пауза» в начале, ART=номер исполнителя (artist).
# Медленные кадры (> 30 мс) — строками «медленно»: последнее нажатие, через сколько, отрисовка и ожидание pan.
# Обычный XMB останавливается и после замера запускается снова. Строки fps: на кадр мс в lvgl (с копированием
# и pan), копирование, ожидание pan, сон в poll, строк скопировано, кадров дольше 20 мс, самый долгий
rep() { s=""; i=0; while [ $i -lt $2 ]; do s="$s,$1"; i=$((i+1)); done; echo "$s"; }
W=$(rep wait 70)   # 70 шагов по 80 мс — 5.6 с
case ${1:-list} in
list) SC="wait$W$(rep down 5),enter$W$(rep down 70)$(rep up 70)$W"; M="7:покой 10:прокрутка" ;;
grid) SC="wait$W$(rep down 3),enter$W$(rep down 70)$(rep up 70)$W"; M="7:покой 10:сетка" ;;
back) C=",enter$(rep wait 10),esc$(rep wait 10)"; SC="wait$W$(rep down 3),enter$(rep wait 20)$(rep down 4)$C$C$C$C$C$C$C$C$C$C$W"
	M="7:покой 10:вход-возврат" ;;
artist) N=${ART:+$(rep down $ART)}   # ART=номер исполнителя — все круги по нему
	C=",enter$(rep wait 15),enter$(rep wait 15),esc$(rep wait 15),esc$(rep wait 15)"
	[ -z "$ART" ] && C="$C,down$(rep wait 4)"
	SC="wait$W${PLAY:+,play}$(rep down 4),enter$(rep wait 20)$N$(rep wait 10)$C$C$C$C$C$C$W"; M="7:покой 10:исполнитель" ;;   # PLAY=1 — с музыкой
cats) C=",left$(rep wait 5),left$(rep wait 5),right$(rep wait 5),right$(rep wait 5),right$(rep wait 5),left$(rep wait 5)"
	SC="wait$W$C$C$C$C$C$C$W"; M="7:покой 10:категории" ;;
opts) SC="wait$W,left$(rep wait 5)$(rep down 3),enter$(rep wait 10)$(rep down 11)$(rep up 11)$(rep down 11)$(rep up 11)$(rep down 11)$(rep up 11),esc$W"
	M="7:покой 10:меню" ;;
opts2) SC="wait$W,left$(rep wait 5)$(rep down 8),enter$(rep wait 10)$(rep down 9)$(rep up 9)$(rep down 9)$(rep up 9)$(rep down 9)$(rep up 9),esc$W"
	M="7:покой 10:меню" ;;
player) C=",nextalbum$(rep wait 8)"; SC="wait$W,enter${PLAY:+,play}$(rep wait 15)$C$C$C$C$C$C$C$C$C$C$C$C$W"; M="7:покой 10:плеер" ;;
player2) C=",nextalbum$(rep wait 8)"; B=",prevalbum$(rep wait 8)"   # туда и обратно: обратно — всё уже в кэшах
	SC="wait$W,enter${PLAY:+,play}$(rep wait 15)$C$C$C$C$C$C$C$C$B$B$B$B$B$B$B$B$W"; M="7:покой 10:плеер" ;;
popen) C=",enter$(rep wait 20),esc$(rep wait 20)"; SC="wait$W${PLAY:+,play}$C$C$C$C$C$C$C$C$W"; M="7:покой 10:плеер" ;;   # раскрыть и свернуть плеер
pidle) SC="wait$W,enter${PLAY:+,play}$W$W$W"; M="9:плеер 15:плеер" ;;
play) SC="wait$W,enter,play$W$W,power$W$W$W,power,play"; M="9:играет 17:экран_погашен 24:экран_погашен" ;;
esac
sh /tmp/stop_xmb.sh > /dev/null
X=/data/mnt/sd_0/.xmb
mkdir -p /tmp/prof_save && cp $X/settings.txt $X/last.txt $X/queue.txt /tmp/prof_save/ 2>/dev/null   # сценарий меняет — вернуть
cd /tmp/xmb
env ${WAVE:+XMB_WAVE=$WAVE} XMB_STEP=80 XMB_SCRIPT="${MUSIC:+$(rep wait 40),play,}$SC" ./xmb /tmp/xmb/fonts > /tmp/prof.log 2>&1 &   # WAVE=номер стиля фона
P=$!
t=0
for m in $M; do
	at=${m%%:*}; name=${m#*:}
	[ $at -gt $t ] && sleep $((at - t))
	echo "$name: $(sh /tmp/cpu.sh xmb 4)"; t=$((at + 4))
done
wait $P
cp /tmp/prof_save/* $X/ 2>/dev/null
grep -E "fps|suspend|медленно|ввод|кадр логики" /tmp/prof.log
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 3
