#ifndef XMB_H
#define XMB_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
	XMB_WHEEL_UP,
	XMB_WHEEL_DOWN,
	XMB_SELECT,
	XMB_BACK,
	XMB_CAT_PREV,
	XMB_CAT_NEXT,
	XMB_NEXT_TRACK,
	XMB_PLAY,
	XMB_POWER,
	XMB_SHADE,      /* открыть/закрыть шторку (на плеере — свайп сверху или пункт меню) */
	XMB_PREV_TRACK, /* трек дольше 3 с — сначала в его начало */
	XMB_NEXT_ALBUM, /* удержание «след. трек»: следующий альбом (папка) очереди */
	XMB_FAV,        /* сердечко в плеере (на ПК — клавиша F) */
	XMB_MENU,       /* действия с пунктом: долгий клик колесом или долгое касание (на ПК — M) */
	XMB_ADD_PL,     /* «+» в плеере — в плейлист (на ПК — клавиша L) */
} xmb_input_t;

/* font_dir — папка с NotoSans-Regular.ttf и NotoSans-SemiBold.ttf */
void xmb_init(const char *font_dir);
void xmb_input(xmb_input_t in);
/* команда сценария по имени (up, down, enter, esc, left, right, play, …): 0 — нет такой */
int xmb_input_name(const char *name);
bool xmb_screen_on(void);
/* пользователь выбрал «Стоковый плеер»: XMB завершается, следующая загрузка — в сток (run_xmb.sh) */
bool xmb_quit_requested(void);
/* батарея села: XMB завершается кодом 4, run_xmb.sh выключает плеер */
bool xmb_power_off_requested(void);
/* после удаления с карты: XMB перезапускается (библиотека перечитывается, место в меню восстанавливается) */
bool xmb_restart_requested(void);
/* перед перезапуском (после USB-накопителя, удаления, возврата карты): запомнить место в меню */
void xmb_restart_prepare(void);
/* сон как в стоке: отсчёт на экране (left — секунды, < 0 — убрать; hint — как отменить),
 * перед сном — пауза, позиция сохранена, экран погашен; время последнего ввода — отмена отсчёта касанием */
void xmb_sleep_countdown(int left, const char *hint);
void xmb_sleep_prepare(void);
void xmb_sleep_end(void);   /* после сна (или неудачной попытки): радио — как было */
uint32_t xmb_last_input(void);
/* USB: подключён компьютер (предложить накопитель); запрос «накопитель» из меню; перед отдачей карты — всё
 * сохранить и закрыть; экран режима накопителя (on = 0 — убрать); выход из него касанием */
void xmb_usb_host(int connected, int offer);
bool xmb_usb_storage_requested(void);
void xmb_storage_prepare(void);
bool xmb_usb_end_requested(void);

/* кнопки на корпусе: платформа распознаёт жест, что делать — решают настройки */
enum { XMB_BTN_NEXT, XMB_BTN_PLAY, XMB_BTN_SCREEN, XMB_BTN_COUNT };
enum { XMB_G_SHORT, XMB_G_LONG, XMB_G_DOUBLE, XMB_G_COUNT };
void xmb_button(int button, int gesture);
bool xmb_button_has_double(int button);   /* ждать ли второго нажатия (иначе короткое — сразу) */
/* глубокий сон: через сколько мс погашенного экрана можно усыплять устройство (0 — нельзя: играет музыка,
 * идёт таймер сна или сон выключен в настройках); xmb_wake — после пробуждения */
uint32_t xmb_sleep_after_ms(void);
void xmb_wake(void);
void xmb_screen_broken(void);   /* вывод кадров не восстанавливается */

/* реализуются платформой (main_host.c / main_device.c) */
int platform_battery(int *charging);   /* заряд в %, -1 — неизвестно */
int platform_battery_mv(void);         /* напряжение, мВ; -1 — неизвестно */
void platform_brightness(int percent);

#endif
