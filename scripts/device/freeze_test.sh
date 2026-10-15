# freeze_test.sh — будит ли кнопка на АЦП («пред. трек») из лёгкого сна (freeze, suspend-to-idle).
# Запасной будильник RTC через 40 с. Пробуждение раньше и рост счётчика adc — значит, будит.
irq() { grep -E " $1\$" /proc/interrupts | awk '{print $2}'; }
echo "через 5 с — сон freeze; во сне нажмите «пред. трек»"
sleep 5
A1=$(irq adc); G1=$(irq KEY_POWER)
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo +40 > /sys/class/rtc/rtc0/wakealarm
t0=$(date +%s)
echo freeze > /sys/power/state
t1=$(date +%s)
echo 0 > /sys/class/rtc/rtc0/wakealarm
echo "проснулся через $((t1 - t0)) с; adc +$(( $(irq adc) - A1 )), колесо +$(( $(irq KEY_POWER) - G1 ))"
dmesg | grep -i -E "wakeup|freeze|PM:" | tail -8
