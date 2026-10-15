# lcd_reload.sh <module.ko> — заменить только драйвер панели (soc_fb не трогать: его rmmod роняет систему).
# Лог — ещё и на карту (hiby_dev/lcd_reload.log): если система повиснет, после перезагрузки видно, на каком шаге.
# Откат: reboot.
KO=$1
LOG=/data/mnt/sd_0/hiby_dev/lcd_reload.log
say() { echo "$*"; echo "$(date +%T) $*" >> $LOG; sync; }
: > $LOG
killall hiby_player.sh 2>/dev/null
killall hiby_player 2>/dev/null
sleep 1
say "module: $KO"
say "before: $(cat /sys/class/graphics/fb0/modes)"
say "rmmod lcd"; rmmod lcd_st77903_8bit; say "rc=$?"
say "insmod $KO"
insmod $KO gpio_lcd_power_en=-1 gpio_lcd_rst=PB05 gpio_spi_cs=PB04 gpio_spi_scl=PA00 gpio_spi_sda=PA01 vcc_regulator_name=bldo1 vccio_regulator_name=bldo2
say "insmod rc=$?"
sleep 1
say "after: $(cat /sys/class/graphics/fb0/modes)"
echo 0 > /sys/class/backlight/backlight_pwm0/brightness
echo 100 > /sys/class/backlight/backlight_pwm0/brightness
dmesg | tail -15 >> $LOG
sync
say "done"
