killall hiby_player.sh 2>/dev/null
killall hiby_player 2>/dev/null
sleep 1
echo 101 > /sys/class/backlight/backlight_pwm0/brightness
chmod +x /tmp/touchtest
cat /sys/class/graphics/fb0/modes
/tmp/touchtest
echo "touchtest exit $?"
