# uac_probe.sh — USB-ЦАП: функция uac_sa.a в стоковом android0 (вместо mass_storage) на 15 с, потом всё как было; лог — /tmp/uac_probe.log
L=/tmp/uac_probe.log
G=/sys/kernel/config/usb_gadget
A0=$G/android0
U=$(ls /sys/class/udc)
dmesg -c > /dev/null
echo "$(date +%T) старт; android0: $(ls $A0/configs/c.1 | tr '\n' ' ')" > $L
killall adbd; sleep 1
echo "" > $G/adb_demo/UDC 2>> $L
mkdir $A0/functions/uac_sa.a 2>> $L
echo 48000 > $A0/functions/uac_sa.a/c_srate 2>> $L
ms=$(ls $A0/configs/c.1 | grep mass_storage)
[ -n "$ms" ] && rm $A0/configs/c.1/$ms
ln -s $A0/functions/uac_sa.a $A0/configs/c.1/ 2>> $L
echo $U > $A0/UDC 2>> $L
echo "привязан: $(cat $A0/UDC)" >> $L
sleep 14
echo "$(date +%T) /dev/snd: $(ls /dev/snd | tr '\n' ' ')" >> $L
ls /proc/asound >> $L 2>&1
dmesg >> $L
echo "" > $A0/UDC 2>> $L
rm $A0/configs/c.1/uac_sa.a
rmdir $A0/functions/uac_sa.a 2>> $L
[ -n "$ms" ] && ln -s $A0/functions/${ms%.*}.0 $A0/configs/c.1/$ms 2>> $L
adbd > /dev/null 2>&1 < /dev/null &
sleep 1
echo $U > $G/adb_demo/UDC 2>> $L
echo "$(date +%T) adb вернул; android0: $(ls $A0/configs/c.1 | tr '\n' ' ')" >> $L
