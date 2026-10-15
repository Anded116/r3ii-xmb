# r3ii-xmb

Своя оболочка для плеера **HiBy R3II (2025)** вместо стокового `hiby_player`: интерфейс в духе XMB
(PSP/PS3) с плеером в стиле Walkman. LVGL 9, прямой вывод в framebuffer, управление колесом, кнопками и тачем.

Что умеет: библиотека (альбомы, исполнители, папки, плейлисты, избранное, поиск), очередь и «играть следующим»,
gapless и кроссфейд, FLAC/MP3/WAV/ALAC/AAC, ReplayGain, эквалайзер, обложки с кэшем, 11 анимированных фонов,
сон и пробуждение, режим USB-накопителя, языки RU/EN/ES. Стоковая прошивка остаётся на месте: XMB запускается
с карты памяти.

## Устройство репозитория

| Путь | Что там |
|---|---|
| `apps/xmb` | сам XMB: `src/` — интерфейс, плеер, звук, библиотека; `src/waves/` — фоны; `tools/wavetest.c` — стенд фонов |
| `apps/xmb/lvgl` | LVGL v9.6.0 (подмодуль) + патч из `patches/` |
| `apps/xmb/third_party` | dr_libs, stb_image, TJpgDec, tinyalsa, FAAD2, MP4/ALAC из Sonix — источники и лицензии в их папках |
| `apps/{fbtest,touchtest,inject}` | первые нативные тесты экрана и ввода; `inject` — запись нажатий в устройство ввода (evdev) |
| `scripts/device` | скрипты для плеера: автозапуск, запуск/остановка, замеры (`prof_xmb.sh`), снимки экрана (`shot_xmb.sh`) |
| `scripts/*.sh`, `upt.py` | распаковка и сборка образа прошивки `.upt` (только инструменты — сама прошивка не входит) |
| `overlay` | что добавляется в образ прошивки: `S95sd_autorun` запускает `hiby_dev/autorun.sh` с карты |
| `NOTES.md`, `DESIGN.md` | заметки по железу и прошивке, замеры, решения по интерфейсу |

## Сборка

Собирается в WSL (Ubuntu) от root.

```bash
git clone --recursive https://github.com/Anded116/r3ii-xmb.git
cd r3ii-xmb/apps/xmb
git -C lvgl apply ../../../patches/lvgl-9.6.0-rgb565-opa-fill.patch
./build.sh host     # окно SDL2 на ПК (WSLg), нужен libsdl2-dev
./build.sh device   # статический бинарник для плеера: build/xmb.stripped
```

Для `device` нужен тулчейн `mipsel-unknown-linux-musl` (cross-tools/musl-cross, GCC с hard-float) в
`/root/toolchains/mipsel-unknown-linux-musl` — путь задан в `Makefile` (`TC`).

Патч LVGL убирает невстроенный вызов смешивания цвета на каждый пиксель полупрозрачной заливки RGB565
(на этом процессоре — заметная доля кадра).

## Установка на плеер

1. Прошивка с `overlay/etc/init.d/S95sd_autorun` (собирается `scripts/build.sh` из стокового `.upt`)
   при загрузке запускает `hiby_dev/autorun.sh` с карты памяти: тот поднимает adb и запускает XMB.
2. `scripts/deploy_xmb.ps1` кладёт бинарник, шрифты и скрипты в `hiby_dev/` на карте (по adb).
   Ожидает `adb.exe` в `tools/platform-tools/` — Android SDK Platform-Tools, в репозиторий не входит.

Прошивки, образы и инструменты HiBy в репозиторий не входят.

## Лицензии

Чужой код — со своими лицензиями (см. `third_party/*`). Файлы Sonix (`mp4.c`, `alacdec.c`) — GPL-3.0,
поэтому собранный XMB распространяется на условиях GPL-3.0. Шрифты: Noto Sans и Font Awesome 4 — SIL OFL 1.1.
