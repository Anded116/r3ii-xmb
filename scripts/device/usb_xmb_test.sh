# usb_xmb_test.sh — проба режима накопителя в самом XMB по сценарию: Настройки → «USB-накопитель», затем XMB ждёт,
# пока компьютер извлечёт диск. После выхода — обычный XMB. Запускать отвязанным (setsid): adb на это время пропадает.
# Журналы: /tmp/usbx.log (сценарий), /tmp/xmb_usb.log (XMB, пока карта у компьютера), копия — hiby_dev/usbx.log
W=""; i=0; while [ $i -lt 200 ]; do W="$W,wait"; i=$((i+1)); done
D=""; i=0; while [ $i -lt 17 ]; do D="$D,down"; i=$((i+1)); done
sh /tmp/stop_xmb.sh > /dev/null
cd /tmp/xmb
XMB_STEP=300 XMB_SCRIPT="wait,wait,wait,wait,wait,wait,wait,wait,wait,wait,left,wait$D,wait,enter$W" ./xmb /tmp/xmb/fonts > /tmp/usbx.log 2>&1
echo "xmb exit $?" >> /tmp/usbx.log
cat /tmp/xmb_usb.log >> /tmp/usbx.log 2>/dev/null
mount | grep sd_0 >> /tmp/usbx.log
cp /tmp/usbx.log /data/mnt/sd_0/hiby_dev/usbx.log 2>/dev/null
setsid nohup sh /tmp/run_xmb.sh > /dev/null 2>&1 < /dev/null &
sleep 3
