# bl_blink.sh — чередовать яркость 60 / 100 трижды по 1.5 с
B=/sys/class/backlight/backlight_pwm0/brightness
for v in 60 100 60 100 60 100; do echo $v > $B; echo "now $v"; sleep 1.5; done
