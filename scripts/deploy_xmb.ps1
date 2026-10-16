# deploy_xmb.ps1 — отправить XMB и служебные скрипты в /tmp плеера (после ребута /tmp пуст) и установить на карту:
# hiby_dev/xmb — то, что autorun.sh при загрузке копирует в /tmp и запускает вместо стокового плеера
$adb = "$PSScriptRoot\..\tools\platform-tools\adb.exe"
$r = "$PSScriptRoot\.."
$sd = "/data/mnt/sd_0/hiby_dev"
& $adb shell "mkdir -p /tmp/xmb/fonts $sd/xmb/fonts"
$fonts = @("NotoSans-Regular.ttf", "NotoSans-SemiBold.ttf", "FontAwesome4.ttf")
$scripts = @("cpu.sh", "bench_xmb.sh", "prof_xmb.sh", "run_xmb.sh", "stop_xmb.sh", "stop_stock.sh", "xmb_start.sh")
$files = @(
	@("$r\apps\xmb\build\xmb.stripped", "/tmp/xmb/xmb"),
	@("$r\apps\xmb\build\xmb.stripped", "$sd/xmb/xmb"),
	@("$r\apps\inject\inject", "/tmp/inject"),
	@("$r\apps\inject\inject", "$sd/xmb/inject"),
	@("$r\scripts\device\autorun.sh", "$sd/autorun.sh")
)
foreach ($f in $fonts) { $files += , @("$r\apps\xmb\res\fonts\$f", "/tmp/xmb/fonts/"); $files += , @("$r\apps\xmb\res\fonts\$f", "$sd/xmb/fonts/") }
foreach ($s in $scripts) { $files += , @("$r\scripts\device\$s", "/tmp/"); $files += , @("$r\scripts\device\$s", "$sd/xmb/") }
foreach ($f in $files) { & $adb push $f[0] $f[1] | Out-Null }
& $adb shell "chmod +x /tmp/xmb/xmb /tmp/inject; sync; ls /tmp/xmb /tmp/xmb/fonts $sd/xmb"
# прошивка с xmb_boot.sh запускает XMB из внутренней памяти — обновить и её
& $adb shell "[ -x /usr/bin/xmb_boot.sh ] && mkdir -p /usr/data/xmb/fonts && cp $sd/xmb/xmb $sd/xmb/inject $sd/xmb/*.sh /usr/data/xmb/ && cp $sd/xmb/fonts/* /usr/data/xmb/fonts/ && chmod +x /usr/data/xmb/xmb /usr/data/xmb/inject && sync && echo 'внутренняя память обновлена'"
