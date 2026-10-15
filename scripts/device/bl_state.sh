# bl_state.sh [значение] — показать состояние подсветки, при аргументе — записать яркость
B=/sys/class/backlight/backlight_pwm0
[ -n "$1" ] && echo "$1" > $B/brightness
for f in brightness actual_brightness bl_power max_brightness; do echo "$f: $(cat $B/$f)"; done
