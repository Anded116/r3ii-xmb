/*
 * XMB для HiBy R3II 2025 (320×480, портрет). Решения по навигации — DESIGN.md.
 *
 * Ряд категорий → пункты категории → вложенные уровни любой глубины (как на PSP: каждый уровень
 * сдвигает предыдущие влево в цепочку иконок). Поверх — список вариантов, шторка быстрых
 * настроек и полноэкранный плеер.
 */
#define _GNU_SOURCE   /* nftw */
#include "xmb.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl/lvgl.h"
#include "audio.h"
#include "cover.h"
#include "filesave.h"
#include "library.h"
#include "i18n.h"
#include "playlist.h"
#include "xmb_bg.h"
#include "xmb_player.h"
#include "bt.h"
#include "wifi.h"
#include "podcast.h"
#include <pthread.h>
#include <strings.h>
#include <errno.h>
#include <ftw.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdarg.h>
#include <unistd.h>

#define SCR_W 320
#define SCR_H 480

/* размер интерфейса меню: пресет из настроек. Иконки — только размеры встроенных шрифтов
 * (14/16/20/24/28/32/40): другой размер рисуется через масштаб, а это слой при каждой перерисовке */
typedef struct {
	int cat_x;        /* центр текущей колонки; левее видна часть предыдущей категории */
	int cat_y;        /* центр иконок категорий */
	int cat_step, sel_y, above_y;
	int deep_sel_y;   /* выбранный во вложенных уровнях — на высоте первого уровня (звено цепочки в ряд), выше — подряд */
	int row, text_dx; /* шаг строк; от центра иконки до текста */
	int icon, icon_sel, cat_icon, cat_icon_sel;
	int f_title, f_item, f_sub;
} ui_size_t;
static const ui_size_t ui_sizes[] = {
	{ 70, 88, 62, 160, 44, 160, 42, 24, 14, 20, 20, 28, 15, 13, 11 },
	{ 78, 96, 70, 176, 44, 176, 48, 28, 16, 24, 24, 32, 17, 15, 12 },
	{ 92, 104, 84, 196, 40, 196, 56, 30, 20, 28, 28, 40, 19, 17, 13 },
	{ 100, 112, 92, 214, 40, 214, 64, 34, 24, 32, 32, 40, 21, 19, 14 },
};
static const char *const size_names[] = { "Очень мелкий", "Мелкий", "Обычный", "Крупный", NULL };
static const ui_size_t *sz = &ui_sizes[1];
#define CAT_X (sz->cat_x)
#define CAT_Y (sz->cat_y)
#define CAT_STEP (sz->cat_step)
#define ITEM_SEL_Y (sz->sel_y)
#define ITEM_ABOVE_Y (sz->above_y)
#define DEEP_SEL_Y (sz->deep_sel_y)
#define ROW (sz->row)
#define TEXT_DX (sz->text_dx)
#define NLET 48        /* букв в алфавитном указателе */
#define INDEX_MIN 30   /* указатель — у списков от стольких пунктов */
#define IDX_W 18       /* ширина полосы указателя */
#define GLOW 72       /* размер пятна свечения под выбранным */
#define GLOW_PAD 56   /* у обложки пятно больше её на столько */
#define POOL 16       /* строк на колонку: привязываются к видимым пунктам, списки любой длины */
#define ROW_COVER 34  /* обложка в строке */
#define GRID_COLS 3    /* сетка альбомов: колонки, шаг ячейки, обложка, центр текущего ряда */
#define GRID_CELL 86
#define GRID_COVER 70
#define GRID_TOP (TOP_H + 2)    /* верх первого ряда сетки: 4 ряда с подписями — до мини-плеера */
#define BIG_ROW 72     /* «список с обложками»: шаг строки и обложка */
#define BIG_COVER 60
#define MAX_DEPTH 6
#define OPT_ROW 40
#define OPT_W 232   /* наибольшая ширина бокового меню; обычно — по самой длинной строке */
#define BAT_FONT_PX 11   /* процент в батарейке */
#define MAX_OPTS 16
#define MINI_H 52
#define TOP_H 28
#define SHADE_H 304
#define SEG 30         /* сегментов в ползунке шторки */
#define ACCENT 0xff7a1a /* оранжевый Walkman */

enum { A_NONE, A_TRACK, A_EQ, A_BTN_RESET, A_SEARCH, A_STOCK, A_USB, A_NEWPL, A_BTDEV, A_BTSCAN, A_WIFINET, A_WIFISCAN, A_PODUPDATE };
enum { T_NONE, T_TAP, T_HCAT, T_VLIST, T_SWIPE_UP, T_PLAYER, T_PULL, T_SHADE, T_BACKSWIPE, T_INDEX, T_MINI, T_SEEK, T_EQ };

/* ---------- данные ---------- */

typedef struct node {
	const char *icon, *title, *sub;
	const struct node *kids;        /* вложенный уровень */
	int nkids;
	const char *const *opts;        /* варианты выбора, NULL-terminated */
	int *val;                       /* текущий вариант */
	void (*apply)(void);
	const uint32_t (*swatch)[2];    /* образцы цвета для вариантов (тема) */
	int action, track;              /* track — индекс трека библиотеки */
	const char *cover;              /* файл обложки альбома */
	const char *key;                /* по чему отсортирован список: первая буква — в указатель */
	void (*commit)(void);           /* варианты: действие по выбору (не при предпросмотре) */
	int grid;                       /* вложенный уровень — сетка обложек (альбомы) */
} node_t;

typedef struct {
	const char *icon, *title;
	const node_t *items;
	int count;
} cat_t;

typedef struct {
	lv_obj_t *icon, *title, *sub, *img;
	int idx;            /* пункт, к которому привязана строка, -1 — свободна */
	const char *icon_txt;
} row_t;

typedef struct {
	lv_obj_t *cont;
	row_t rows[POOL];
	int pool_ready, count, sel;
	double pos;
	const node_t *nodes;
	int nlet, let_at[NLET];   /* алфавитный указатель: буква и первый пункт на неё */
	char let[NLET][8];
	int grid;                 /* сетка: ряды по GRID_COLS, rpos — плавная позиция ряда */
	int big;                  /* список с крупными обложками и тремя строками текста */
	double rpos;
} column_t;

#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define TRK(t, i)            { LV_SYMBOL_AUDIO, t, NULL, NULL, 0, NULL, NULL, NULL, NULL, A_TRACK, i }
#define DIR(ic, t, sub, k)   { ic, t, sub, k, N(k), NULL, NULL, NULL, NULL, A_NONE, 0 }
#define OPT(ic, t, o, v, ap) { ic, t, NULL, NULL, 0, o, v, ap, NULL, A_NONE, 0 }
#define ACT(ic, t, sub, a)   { ic, t, sub, NULL, 0, NULL, NULL, NULL, NULL, a, 0 }

static const char *const theme_names[] = {
	"Январь", "Февраль", "Март", "Апрель", "Май", "Июнь",
	"Июль", "Август", "Сентябрь", "Октябрь", "Ноябрь", "Декабрь", NULL,
};
static const uint32_t theme_colors[12][2] = {
	{ 0x3a4250, 0x8d97a8 }, { 0x5a4512, 0xb59a3e }, { 0x2b4a14, 0x7aa83a }, { 0x5a2742, 0xc77aa0 },
	{ 0x0f4a2a, 0x3fae6b }, { 0x3a1f5c, 0x9a6fd0 }, { 0x0b4a52, 0x35b5c0 }, { 0x0d2a5e, 0x4f86d8 },
	{ 0x4a1f5a, 0xa35cc0 }, { 0x5a3a08, 0xc98f2a }, { 0x4a2a16, 0x9a6844 }, { 0x5a0f14, 0xc4373e },
};
static const char *const wave_names[] = { "Волна PS3", "Лента", "Частицы", "Спектр", "Шёлк", "Сияние", "Звёздная пыль",
					    "Пульс", "Круги", "Волна PSP", "Выключен", NULL };
static const char *const onoff[] = { "Включено", "Выключено", NULL };
enum { MINI_CLEAR, MINI_DARK, MINI_BLACK, MINI_THEME, MINI_CARD, MINI_FRAME };
static const char *const mini_names[] = { "Прозрачный", "Тёмный", "Чёрный", "Цвет темы", "Карточка",
					  "Чёрный со скруглением", NULL };
#define EDGE_MAX 8
static const char *const edge_names[] = { "0 px", "1 px", "2 px", "3 px", "4 px", "5 px", "6 px", "7 px", "8 px", NULL };
static const char *const bright_names[] = { "10%", "20%", "30%", "40%", "50%", "60%", "70%", "80%", "90%", "100%", NULL };
static const char *const sleep_names[] = { "1 минута", "5 минут", "10 минут", "30 минут", "Выключен", NULL };
static const int sleep_min[] = { 1, 5, 10, 30, 0 };
#define SLEEP_OFF 4
#define SLEEP_FADE_MS 15000   /* таймер сна: громкость плавно уходит в ноль за это время */
static const char *const screen_names[] = { "15 секунд", "30 секунд", "1 минута", "2 минуты", "5 минут", "Никогда", NULL };
static const int screen_sec[] = { 15, 30, 60, 120, 300, 0 };
static const char *const dev_sleep_names[] = { "Через 1 минуту", "Через 5 минут", "Выключен", NULL };
static const int dev_sleep_sec[] = { 60, 300, 0 };
static const char *const album_views[] = { "Сетка", "Список", "Список с обложками", NULL };
static const char *const player_bg_names[] = { "Затемнение", "Цвет обложки", NULL };
static const char *const gain_names[] = { "Высокое", "Среднее", "Низкое", NULL };
static const char *const rg_names[] = { "Выключен", "По трекам", "По альбомам", NULL };
static const char *const lang_names[] = { "Русский", "English", "Español", NULL };   /* каждый — на своём языке */
static const char *const xfade_names[] = { "Выключен", "2 с", "4 с", "6 с", NULL };
/* действия кнопок на корпусе */
enum { BA_NONE, BA_PLAY, BA_NEXT, BA_PREV, BA_NEXT_ALBUM, BA_PREV_ALBUM, BA_FWD, BA_REW, BA_SHUFFLE, BA_REPEAT, BA_FAV,
       BA_SCREEN, BA_COUNT };
static const char *const btn_act_names[] = { "Ничего", "Пауза / воспроизведение", "Следующий трек", "Предыдущий трек",
					     "Следующий альбом", "Предыдущий альбом", "Вперёд на 10 с", "Назад на 10 с",
					     "Случайный порядок", "Повтор", "Избранное", "Экран", NULL };
static const int btn_default[XMB_BTN_COUNT][XMB_G_COUNT] = {
	{ BA_NEXT, BA_NEXT_ALBUM, BA_NONE },   /* «след. трек»: нажатие, удержание, два нажатия */
	{ BA_PLAY, BA_PREV, BA_NONE },         /* «пауза» */
	{ BA_SCREEN, BA_NONE, BA_NONE },       /* «пред. трек» (экран) */
};
static const char *const eq_onoff[] = { "Выключен", "Включён", NULL };
static const char *const eq_presets[] = { "Ровный", "Басы", "Рок", "Поп", "Электроника", "Джаз", "Классика", "Вокал",
					  "Высокие", "Свой", NULL };
#define EQ_CUSTOM 9
static const signed char eq_preset_db[EQ_CUSTOM][EQ_BANDS] = {
	{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },      { 6, 5, 4, 2, 0, 0, 0, 0, 0, 0 },     { 4, 3, 2, 0, -1, -1, 0, 2, 3, 4 },
	{ -1, 0, 2, 3, 3, 2, 0, -1, -1, -1 },  { 5, 4, 1, 0, -1, 1, 0, 1, 4, 5 },    { 3, 2, 1, 2, -1, -1, 0, 1, 2, 3 },
	{ 3, 2, 1, 0, 0, 0, -1, -1, 1, 2 },    { -2, -2, -1, 1, 3, 3, 2, 1, 0, -1 }, { 0, 0, 0, 0, 0, 0, 1, 3, 4, 5 },
};
static const char *const eq_labels[EQ_BANDS] = { "31", "62", "125", "250", "500", "1k", "2k", "4k", "8k", "16k" };
static const char *const bt_onoff[] = { "Включён", "Выключен", NULL };
static const char *const codecs[] = { "Авто", "LDAC", "aptX HD", "aptX", "AAC", "SBC", NULL };

/* ---------- состояние ---------- */

static struct {
	lv_font_t *f_title, *f_item, *f_sub, *f_clock;
	lv_font_t *m_title, *m_item, *m_sub, *f_letter;   /* шрифты меню — по размеру интерфейса */
	lv_font_t *f_icons, *f_tile;                      /* Font Awesome 4.7: сердечко, плитки шторки */
	int dy_title, dy_title_sel, dy_sub, h_title, h_sub;
	lv_obj_t *idx_strip, *bubble, *bubble_lbl;
	double bubble_t;
	uint32_t wheel_ms;
	double wheel_rate, seek_preview;   /* seek_preview ≥ 0 — палец тянет полосу прогресса */
	int resume;                        /* последний трек прошлого запуска выбран, но ещё не открыт */
	double resume_pos;
	uint32_t last_saved_ms;
	char settings_path[300];
	uint32_t settings_ms;
	unsigned settings_sig;
	lv_obj_t *cat_icons[8], *cat_title, *mini_icon, *mini_text, *mini_bar, *mini_track, *off;
	int mini_bar_w;   /* ширина полосы прогресса мини-плеера */
	lv_obj_t *tb, *tb_left, *tb_right, *tb_bat, *tb_act, *hint[2], *bhint, *bhint_lbl;
	int bat, bat_chg;
	lv_font_t *f_bat;
	lv_obj_t *opt_panel, *opt_rows[MAX_OPTS], *opt_checks[MAX_OPTS], *opt_sw[MAX_OPTS];
	column_t cat_col[8], stack[MAX_DEPTH];   /* stack[d] — вложенный уровень d (с 1) */
	int cat, at_cats, depth, screen_on, glow_x, glow_y, glow_sz;
	int act_kind;             /* фоновая работа в статус-баре: 0 — нет, 1 — скан библиотеки, 2 — подкасты */
	double act_frac, act_phase;   /* доля (−1 — неизвестна: бежит отрезок) и фаза бегущего отрезка */
	char act_text[64];
	double glow_k, level;
	lv_obj_t *glow, *main, *mini, *mini_thumb, *mini_artist, *mini_time;
	double cat_pos, depth_t, focus_t, hint_t[2], bhint_t, slide;
	/* настройки */
	int theme, wave, touch_off, bright, sleep, bt_off, wifi_off, codec, shuffle, repeat, usb_dac, eq_on, ui_size;
	int screen_off, album_view, dev_sleep, gain, rg_mode, xfade, lang, player_bg, kb_start, kb_langs;
	int mini_style, edge_l, edge_r;   /* вид мини-плеера; сколько пикселей экрана слева и справа закрывает рамка */
	int btn[XMB_BTN_COUNT][XMB_G_COUNT];   /* действие кнопки на жест */
	/* эквалайзер: пресет, свои полосы (дБ + 12, чтобы хранить в файле настроек), редактор */
	int eq_preset, eq_custom[EQ_BANDS], eq_edit, eq_band, eq_adjust;
	lv_obj_t *eq_panel, *eq_title;
	uint32_t input_ms, sleep_end, active_ms;
	int ab;                 /* повтор A-B: 0 — нет, 1 — отмечена A, 2 — крутится */
	double ab_a, ab_b;    /* последнее касание или кнопка; конец таймера сна (0 — не идёт) */
	/* варианты */
	const node_t *opt_item;
	int opt_open, opt_count, opt_sel, opt_orig, opt_w;
	double opt_pos, opt_t, opt_sub;   /* opt_sub 1 → 0: вложенное меню въезжает справа поверх прежнего */
	/* шторка */
	lv_obj_t *sh_panel, *sh_bg[8], *sh_icon[8], *sh_label[8], *sh_slider[2];
	int sh_open, sh_drag;   /* sh_drag — палец на ползунке: 1 + номер */
	double sh_t;
	/* воспроизведение: очередь — список, из которого запущен трек */
	int track, playing, volume, ended_seen;
	double pos, dur;
	const node_t *queue;
	int qlen, qi;
	int next_qi, seq;   /* следующий в очереди (заказан движку заранее, −1 — нет) и счётчик его переходов */
	int audio_gen;      /* трек открыт потоком звука: формат (кодек, бит, частота) — уже его */
	int up_next;        /* сколько треков сразу за текущим поставлено «играть следующим» — их не перемешивать */
	double seek_hold;   /* перемотали сюда: позиция из движка — только когда он до неё дошёл (иначе шкала дёргается) */
	uint32_t seek_ms;
	track_t np;
	char np_format[48];
	lv_obj_t *mini_img;
	library_t *lib;
	/* плейлисты: [0] — «Избранное» */
	playlist_t *pls;
	int npl, pl_choice;
	char pl_dir[300];
	const char *pl_names[MAX_OPTS + 1];
	node_t pl_menu;
	lv_obj_t *toast, *toast_lbl;
	double toast_t;
	int *add_tracks, nadd;                 /* что добавляется в плейлист из меню */
	lv_obj_t *kb_panel, *kb_ta;            /* экранная клавиатура: имя плейлиста или поиск (kb_pl < 0) */
	lv_obj_t *kb_info, *kb_prev[4];        /* поиск: сколько найдено и первые находки */
	int kb_pl;
	int *album_order, *album_pos;   /* порядок альбомов для карусели плеера */
	node_t **album_nodes;           /* треки каждого альбома — очередь при переходе по карусели */
	/* тач */
	int t_mode, t_menu_off;
	lv_point_t t_start;
	uint32_t t_ms;
	double t_start_pos;
	int idx_sel;                /* выбор до касания указателя: свайп влево с него — назад, выбор возвращается */
	double idx_pos, idx_rpos;
	/* бросок: скорость — по движению пальца за последние ~100 мс (тач 33 Гц — по двум точкам скачет);
	 * после отпускания список едет сам и тормозит трением, затем докатывается до пункта */
	struct { uint32_t t; int y; } t_hist[8];
	int t_nhist;
	int kin;              /* 1 — колонка, 2 — меню выбора */
	double kin_v;         /* пунктов (рядов сетки) в секунду */
	uint32_t last_ms;
} s;

static void apply_theme(void);
static void apply_mini(void);
static void apply_edges(void);
static void corners_place(void);
static void apply_wave(void);
static void apply_bright(void);
static void apply_size(void);
static void apply_gain(void);
static void apply_rg(void);
static void eq_apply(void);
static void eq_open(void);
static void sleep_start(void);
static void player_more(void);
static void ab_reset(void);
static void q_add(const int *tracks, int n, int next);
static void q_remove(int pos);
static void apply_xfade(void);
static void apply_lang(void);
static void plan_next(void);
static void screen(int on);
static const char *music_root(void);
static void settings_load(void);
static void settings_poll(void);
static void options_open(const node_t *it);
static void playlists_build(void);
static void last_save(void);
static void save_flush(void);
static void last_file(char *out, size_t cap);
static void last_load(void);
static void size_metrics(void);
static void font_xheight(const lv_font_t *f, int *asc, int *xh);
static void about_fill(void);
static void battery_guard(void);
static void lyrics_refresh(void);
static void lyrics_poll(void);
static void pod_labels(void);
static void podcast_poll(void);
static void toast(const char *fmt, ...);
static void seek_to(double sec);
static void slide_tick(double dt);
static void slide_open(const char *icon, const char *title, const char *sub, const char *hint, int dir,
		       const char *cancel_text, void (*done)(void), void (*cancelled)(void));

/* устройства Bluetooth: строит bt_build из снимка bt.c, последний пункт — поиск */
static node_t bt_nodes[BT_MAX + 1];
static bt_dev_t bt_list[BT_MAX];
static char bt_dev_sub[48];
static const char *const codec_bt[] = { NULL, "LDAC", "aptX-HD", "aptX", "AAC", "SBC" };   /* как в codecs */
static void bt_apply(void);
static void bt_codec_apply(void);

/* сети Wi-Fi: строит wifi_build из снимка wifi.c, последний пункт — поиск */
#define KB_WIFI -2   /* клавиатура вводит пароль сети wifi_kb_ssid */
static node_t wifi_nodes[WIFI_MAX + 1];
static wifi_net_t wifi_list[WIFI_MAX];
static char wifi_sub[WIFI_MAX][72], wifi_items_sub[48], wifi_kb_ssid[33];
static void wifi_apply(void);

static const node_t eq_items[] = {
	OPT(LV_SYMBOL_POWER, "Эквалайзер", eq_onoff, &s.eq_on, eq_apply),
	OPT(LV_SYMBOL_LIST, "Пресет", eq_presets, &s.eq_preset, eq_apply),
	ACT(LV_SYMBOL_EDIT, "Настроить полосы", "свой пресет: колесо — полоса, клик — изменить", A_EQ),
};
static const node_t btn_items[] = {
	OPT(LV_SYMBOL_NEXT, "«След. трек»: нажатие", btn_act_names, &s.btn[XMB_BTN_NEXT][XMB_G_SHORT], NULL),
	OPT(LV_SYMBOL_NEXT, "«След. трек»: удержание", btn_act_names, &s.btn[XMB_BTN_NEXT][XMB_G_LONG], NULL),
	OPT(LV_SYMBOL_NEXT, "«След. трек»: два нажатия", btn_act_names, &s.btn[XMB_BTN_NEXT][XMB_G_DOUBLE], NULL),
	OPT(LV_SYMBOL_PLAY, "«Пауза»: нажатие", btn_act_names, &s.btn[XMB_BTN_PLAY][XMB_G_SHORT], NULL),
	OPT(LV_SYMBOL_PLAY, "«Пауза»: удержание", btn_act_names, &s.btn[XMB_BTN_PLAY][XMB_G_LONG], NULL),
	OPT(LV_SYMBOL_PLAY, "«Пауза»: два нажатия", btn_act_names, &s.btn[XMB_BTN_PLAY][XMB_G_DOUBLE], NULL),
	OPT(LV_SYMBOL_PREV, "«Пред. трек»: нажатие", btn_act_names, &s.btn[XMB_BTN_SCREEN][XMB_G_SHORT], NULL),
	OPT(LV_SYMBOL_PREV, "«Пред. трек»: два нажатия", btn_act_names, &s.btn[XMB_BTN_SCREEN][XMB_G_DOUBLE], NULL),
	ACT(LV_SYMBOL_REFRESH, "Как было", "вернуть назначения по умолчанию", A_BTN_RESET),
};
static const char *const wifi_onoff[] = { "Включён", "Выключен", NULL };
static node_t wifi_items[] = {
	OPT(LV_SYMBOL_WIFI, "Wi-Fi", wifi_onoff, &s.wifi_off, wifi_apply),
	{ LV_SYMBOL_LIST, "Сети", wifi_items_sub, wifi_nodes, 1 },
};
static node_t bt_items[] = {
	OPT(LV_SYMBOL_BLUETOOTH, "Bluetooth", bt_onoff, &s.bt_off, bt_apply),
	{ LV_SYMBOL_LIST, "Устройства", bt_dev_sub, bt_nodes, 1 },
	OPT(LV_SYMBOL_AUDIO, "Кодек", codecs, &s.codec, bt_codec_apply),
};
/* «О системе»: подписи заполняет about_fill при входе */
static char about_sub[5][64];
static node_t about_items[] = {
	{ .icon = LV_SYMBOL_SETTINGS, .title = "XMB", .sub = about_sub[0] },
	{ .icon = LV_SYMBOL_AUDIO, .title = "Библиотека", .sub = about_sub[1] },
	{ .icon = LV_SYMBOL_SD_CARD, .title = "Карта памяти", .sub = about_sub[2] },
	{ .icon = LV_SYMBOL_BATTERY_FULL, .title = "Батарея", .sub = about_sub[3] },
	{ .icon = LV_SYMBOL_HOME, .title = "Устройство", .sub = "HiBy R3II 2025 · Ingenic X1600E" },
	{ .icon = LV_SYMBOL_FILE, .title = "Лицензия", .sub = "GPL-3.0 · github.com/Anded116/r3ii-xmb" },
};

static const node_t sound_items[] = {
	DIR(LV_SYMBOL_BARS, "Эквалайзер", NULL, eq_items),
	OPT(LV_SYMBOL_VOLUME_MAX, "Усиление", gain_names, &s.gain, apply_gain),
	OPT(LV_SYMBOL_SHUFFLE, "ReplayGain", rg_names, &s.rg_mode, apply_rg),
	OPT(LV_SYMBOL_LOOP, "Кроссфейд", xfade_names, &s.xfade, apply_xfade),
};
static const node_t screen_items[] = {
	{ LV_SYMBOL_IMAGE, "Тема оформления", NULL, NULL, 0, theme_names, &s.theme, apply_theme, theme_colors, A_NONE, 0 },
	OPT(LV_SYMBOL_REFRESH, "Фон", wave_names, &s.wave, apply_wave),
	OPT(LV_SYMBOL_EYE_OPEN, "Яркость", bright_names, &s.bright, apply_bright),
	OPT(LV_SYMBOL_EDIT, "Размер интерфейса", size_names, &s.ui_size, apply_size),
	OPT(LV_SYMBOL_IMAGE, "Вид альбомов", album_views, &s.album_view, NULL),
	OPT(LV_SYMBOL_EYE_CLOSE, "Гашение экрана", screen_names, &s.screen_off, NULL),
	OPT(LV_SYMBOL_AUDIO, "Фон плеера", player_bg_names, &s.player_bg, NULL),
	OPT(LV_SYMBOL_AUDIO, "Мини-плеер", mini_names, &s.mini_style, apply_mini),
	OPT(LV_SYMBOL_LEFT, "Рамка: левый край", edge_names, &s.edge_l, apply_edges),
	OPT(LV_SYMBOL_RIGHT, "Рамка: правый край", edge_names, &s.edge_r, apply_edges),
};
static const char *const kb_start_names[] = { "Английская", "Русская", "Последняя", NULL };
static const char *const kb_langs_names[] = { "Английская и русская", "Только английская", NULL };
static const node_t kb_items[] = {
	OPT(LV_SYMBOL_KEYBOARD, "Раскладка при открытии", kb_start_names, &s.kb_start, NULL),
	OPT(LV_SYMBOL_KEYBOARD, "Раскладки", kb_langs_names, &s.kb_langs, NULL),
};
static const node_t control_items[] = {
	DIR(LV_SYMBOL_KEYBOARD, "Кнопки", "что делают кнопки на корпусе", btn_items),
	DIR(LV_SYMBOL_KEYBOARD, "Клавиатура", "раскладка при открытии", kb_items),
	OPT(LV_SYMBOL_GPS, "Сенсорное управление", onoff, &s.touch_off, NULL),
};
static const node_t power_items[] = {
	OPT(LV_SYMBOL_POWER, "Таймер сна", sleep_names, &s.sleep, sleep_start),
	OPT(LV_SYMBOL_POWER, "Сон устройства", dev_sleep_names, &s.dev_sleep, NULL),
};
static const node_t system_items[] = {
	ACT(LV_SYMBOL_REFRESH, "Стоковый плеер", "обновление прошивки", A_STOCK),
	{ LV_SYMBOL_SETTINGS, "О системе", "HiBy R3II 2025 · XMB", about_items, N(about_items) },
};

static const node_t set_items[] = {
	DIR(LV_SYMBOL_AUDIO, "Звук", "эквалайзер, усиление, ReplayGain", sound_items),
	DIR(LV_SYMBOL_IMAGE, "Экран", "тема, фон, яркость, размер", screen_items),
	DIR(LV_SYMBOL_BLUETOOTH, "Bluetooth", NULL, bt_items),
	DIR(LV_SYMBOL_WIFI, "Wi-Fi", wifi_items_sub, wifi_items),
	DIR(LV_SYMBOL_KEYBOARD, "Управление", "кнопки, клавиатура, сенсор", control_items),
	DIR(LV_SYMBOL_POWER, "Питание", "таймер сна, сон устройства", power_items),
	ACT(LV_SYMBOL_USB, "USB-накопитель", "карта — компьютеру по кабелю", A_USB),
	OPT(LV_SYMBOL_KEYBOARD, "Язык · Language", lang_names, &s.lang, apply_lang),
	DIR(LV_SYMBOL_SETTINGS, "Система", "стоковый плеер, о системе", system_items),
};
/* «Музыка» и «Папки» строятся из библиотеки после сканирования */
static char scan_sub[48];
static const node_t scanning[] = { ACT(LV_SYMBOL_REFRESH, "Сканирование библиотеки…", scan_sub, A_NONE) };


/* чаще всего слушают альбомы, избранное и папки: они в шаге от старта («Музыка») */
enum { CAT_SETTINGS, CAT_MUSIC, CAT_PLAYLISTS, CAT_FOLDERS };
static cat_t cats[] = {
	{ LV_SYMBOL_SETTINGS, "Настройки", set_items, N(set_items) },
	{ LV_SYMBOL_AUDIO, "Музыка", scanning, 1 },
	{ LV_SYMBOL_LIST, "Плейлисты", scanning, 1 },
	{ LV_SYMBOL_DIRECTORY, "Папки", scanning, 1 },
};
enum { MUSIC_QUEUE, MUSIC_ALBUMS, MUSIC_ARTISTS, MUSIC_ALL, MUSIC_PODCASTS, MUSIC_SEARCH, MUSIC_COUNT };
static int nmusic;   /* альбомов-музыки: в порядке album_order они первые, за ними — подкасты */
static node_t *pod_nodes;   /* «Подкасты»: [0] — «Обновить», дальше подкасты */
static char pod_sub[96], pod_upd_sub[96];
static node_t music[MUSIC_COUNT];
static node_t *lib_artists, *lib_albums;   /* пункты «Исполнителей» и «Альбомов» — их же показывает поиск */
#define NCAT N(cats)

/* подписи режимов воспроизведения в плеере */
static const char *const repeat_names[] = { "Повтор", "Повтор: все", "Повтор: один", NULL };

/* плитки шторки: значок Font Awesome (без шрифта — символ LVGL) и подпись; включённая — оранжевым кругом */
enum { TL_BT, TL_WIFI, TL_EQ, TL_SLEEP, TL_SHUFFLE, TL_REPEAT, TL_TOUCH, TL_USB, NTILE };
static const struct {
	const char *fa, *sym, *name;
} tiles[NTILE] = {
	{ "\xEF\x8A\x94", LV_SYMBOL_BLUETOOTH, "Bluetooth" },
	{ "\xEF\x87\xAB", LV_SYMBOL_WIFI, "Wi-Fi" },
	{ "\xEF\x87\x9E", LV_SYMBOL_BARS, "Эквалайзер" },
	{ "\xEF\x86\x86", LV_SYMBOL_POWER, "Таймер сна" },
	{ "\xEF\x81\xB4", LV_SYMBOL_SHUFFLE, "Случайно" },
	{ "\xEF\x80\x9E", LV_SYMBOL_LOOP, "Повтор" },
	{ "\xEF\x89\x9A", LV_SYMBOL_GPS, "Сенсор" },
	{ "\xEF\x8A\x87", LV_SYMBOL_USB, "Отдать карту" },
};

/* ---------- утилиты ---------- */

/* данные TTF читаются один раз на файл: все размеры шрифта ссылаются на одну копию */
static lv_font_t *load_font(const char *dir, const char *name, int size)
{
	static struct { char name[64]; void *data; long n; } files[4];
	int k = 0;
	while (k < 4 && files[k].data && strcmp(files[k].name, name))
		k++;
	if (k == 4)
		return NULL;
	if (!files[k].data) {
		char path[256];
		snprintf(path, sizeof(path), "%s/%s", dir, name);
		FILE *f = fopen(path, "rb");
		if (!f) {
			printf("xmb: нет шрифта %s\n", path);
			return NULL;
		}
		fseek(f, 0, SEEK_END);
		long n = ftell(f);
		fseek(f, 0, SEEK_SET);
		void *data = malloc(n);
		if (fread(data, 1, n, f) != (size_t)n) {
			fclose(f);
			free(data);
			return NULL;
		}
		fclose(f);
		snprintf(files[k].name, sizeof(files[k].name), "%s", name);
		files[k].data = data;
		files[k].n = n;
	}
	/* без кернинга: поиск пары букв на каждый глиф заметен в профиле отрисовки, а в меню разницы не видно */
	return lv_tiny_ttf_create_data_ex(files[k].data, files[k].n, size, LV_FONT_KERNING_NONE, LV_TINY_TTF_CACHE_GLYPH_CNT);
}

static lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, const char *text)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, lv_color_white(), 0);
	lv_label_set_text(l, text);
	return l;
}

static lv_obj_t *plain(lv_obj_t *parent)
{
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	return o;
}

static double approach(double cur, double target, double dt, double speed)
{
	double v = cur + (target - cur) * (1.0 - exp(-dt * speed));
	return fabs(target - v) < 0.002 ? target : v;
}

static double clamp01(double v)
{
	return v < 0 ? 0 : v > 1 ? 1 : v;
}

/* сеттеры, которые не трогают объект, если значение не изменилось (иначе LVGL перерисовывает) */
static void set_pos(lv_obj_t *o, int x, int y)
{
	if (lv_obj_get_x(o) != x || lv_obj_get_y(o) != y)
		lv_obj_set_pos(o, x, y);
}

/* прозрачность текста — при отрисовке, а не стилем: у метки любое изменение стиля пересчитывает раскладку текста
 * (замер всех букв, «…»), а при прокрутке прозрачность строк меняется каждый кадр. Значение — в user_data + 1
 * (0 — ещё не задано) */
static void opa_draw_cb(lv_event_t *e)
{
	lv_draw_label_dsc_t *d = lv_draw_task_get_label_dsc(lv_event_get_draw_task(e));
	if (d)
		d->opa = LV_OPA_MIX2(d->opa, (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e)) - 1);
}

static void set_opa(lv_obj_t *o, int opa)
{
	opa = opa < 0 ? 0 : opa > 255 ? 255 : opa;
	intptr_t cur = (intptr_t)lv_obj_get_user_data(o);
	if (cur == opa + 1)
		return;
	if (!cur) {
		lv_obj_set_style_text_opa(o, LV_OPA_COVER, 0);
		lv_obj_set_send_draw_task_events(o, true);
		lv_obj_add_event_cb(o, opa_draw_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
	}
	lv_obj_set_user_data(o, (void *)(intptr_t)(opa + 1));
	lv_obj_invalidate(o);
}

static void set_scale(lv_obj_t *o, int scale)
{
	if (lv_obj_get_style_transform_scale_x(o, 0) != scale)
		lv_obj_set_style_transform_scale(o, scale, 0);
}

/* иконка размера px — ближайшим готовым шрифтом (шаг 2 px), без трансформации: она рендерит объект через
 * отдельный слой при каждой перерисовке, а во время прокрутки размер меняется каждый кадр */
static void set_icon_px(lv_obj_t *o, double px)
{
	static const lv_font_t *const fonts[] = {
		&lv_font_montserrat_14, &lv_font_montserrat_16, &lv_font_montserrat_18, &lv_font_montserrat_20,
		&lv_font_montserrat_22, &lv_font_montserrat_24, &lv_font_montserrat_26, &lv_font_montserrat_28,
		&lv_font_montserrat_30, &lv_font_montserrat_32, &lv_font_montserrat_34, &lv_font_montserrat_36,
		&lv_font_montserrat_38, &lv_font_montserrat_40,
	};
	int i = (int)lround((px - 14) / 2);
	i = i < 0 ? 0 : i > 13 ? 13 : i;
	const lv_font_t *f = fonts[i];
	int scale = 256;
	/* текст метки прижат к верху рамки: глиф центрируется отступом под каждый размер шрифта */
	int pad = (lv_obj_get_style_height(o, 0) - f->line_height) / 2;
	if (lv_obj_get_style_text_font(o, 0) != f)
		lv_obj_set_style_text_font(o, f, 0);
	if (lv_obj_get_style_pad_top(o, 0) != pad)
		lv_obj_set_style_pad_top(o, pad, 0);
	set_scale(o, scale);
}

static void set_hidden(lv_obj_t *o, bool h)
{
	if (lv_obj_is_hidden(o) != h)
		lv_obj_set_hidden(o, h);
}

/* ---------- построение ---------- */

static void layout(void);
static void options_preview(int v);
static void show_hint(int i);
static void shade_set(int open);
static void apply_bright(void);

static const char *node_sub(const node_t *n)
{
	static char buf[8][64];
	static int k;
	char *b = buf[k++ & 7];
	if (n->opts)
		return tx(n->opts[*n->val]);
	if (n->action == A_TRACK && s.lib->albums[s.lib->tracks[n->track].aidx].podcast) {
		/* выпуск: дата (из имени файла), длительность и сколько осталось */
		const lib_track_t *tr = &s.lib->tracks[n->track];
		const char *name = strrchr(tr->path, '/') ? strrchr(tr->path, '/') + 1 : tr->path;
		char date[16] = "";
		if (isdigit((unsigned char)name[0]) && name[4] == '-')
			snprintf(date, sizeof(date), "%.2s.%.2s · ", name + 8, name + 5);
		double left = tr->dur - pod_pos(tr->path);
		if (pod_done(tr->path))
			snprintf(b, 64, "%s%s", date, tx("прослушан"));
		else if (left < tr->dur - 5)
			snprintf(b, 64, tx("%sосталось %d мин"), date, (int)(left / 60 + 0.5));
		else
			snprintf(b, 64, tx("%s%d мин"), date, (int)(tr->dur / 60 + 0.5));
		return b;
	}
	if (n->action == A_TRACK) {
		const lib_track_t *tr = &s.lib->tracks[n->track];
		if (n->cover)
			snprintf(b, 64, "%s · %s · %d:%02d", tx(tr->artist), s.lib->albums[tr->aidx].title, (int)tr->dur / 60,
				 (int)tr->dur % 60);
		else
			snprintf(b, 64, "%s · %d:%02d", tx(tr->artist), (int)tr->dur / 60, (int)tr->dur % 60);
		return b;
	}
	if (n->sub)
		return tx(n->sub);
	if (n->kids) {
		snprintf(b, 64, "%d", n->nkids);
		return b;
	}
	return "";
}

static column_t *col_at(int d)
{
	return d == 0 ? &s.cat_col[s.cat] : &s.stack[d];
}

static column_t *cur_col(void)
{
	return col_at(s.depth);
}

/* первая буква для указателя: латиница и кириллица заглавными, цифры и прочее — «#» */
static void first_letter(const char *s_, char out[8])
{
	const unsigned char *p = (const unsigned char *)(s_ ? s_ : "");
	while (*p && *p < 0x80 && !((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z')))
		p++;   /* кавычки, скобки, пробелы */
	unsigned cp = *p;
	if ((cp & 0xe0) == 0xc0 && p[1])
		cp = (cp & 31) << 6 | (p[1] & 63);
	if (cp >= 'a' && cp <= 'z')
		cp -= 32;
	else if (cp >= 0x430 && cp <= 0x44f)
		cp -= 0x20;
	else if (cp == 0x451 || cp == 0x401)
		cp = 0x415;   /* Ё сортируется вместе с Е */
	if (cp >= 'A' && cp <= 'Z') {
		out[0] = (char)cp;
		out[1] = 0;
	} else if ((cp >= 0x410 && cp <= 0x42f) || cp == 0x401) {
		out[0] = (char)(0xc0 | cp >> 6);
		out[1] = (char)(0x80 | (cp & 63));
		out[2] = 0;
	} else {
		strcpy(out, "#");
	}
}

/* буквы в порядке появления в списке и первый пункт на каждую */
static void column_index(column_t *c)
{
	c->nlet = 0;
	if (c->count < INDEX_MIN)
		return;
	for (int i = 0; i < c->count && c->nlet < NLET; i++) {
		if (!c->nodes[i].key)
			continue;
		char l[8];
		first_letter(c->nodes[i].key, l);
		int k = 0;
		while (k < c->nlet && strcmp(c->let[k], l))
			k++;
		if (k == c->nlet) {
			strcpy(c->let[k], l);
			c->let_at[k] = i;
			c->nlet++;
		}
	}
	if (c->nlet < 2)
		c->nlet = 0;
}

/* буква выбранного пункта (или ближайшего выше с ключом) */
static int cur_letter(const column_t *c)
{
	int i = c->sel;
	while (i > 0 && !c->nodes[i].key)
		i--;
	char l[8];
	first_letter(c->nodes[i].key, l);
	for (int k = 0; k < c->nlet; k++)
		if (!strcmp(c->let[k], l))
			return k;
	return 0;
}

/* текст строк короче, когда справа полоса указателя */
static void column_widths(column_t *c)
{
	int w = SCR_W - CAT_X - TEXT_DX - 6 - (c->nlet ? IDX_W : 0);
	for (int i = 0; i < POOL && c->pool_ready; i++) {
		if (lv_obj_get_width(c->rows[i].title) != w || lv_obj_get_height(c->rows[i].title) != s.h_title)
			lv_obj_set_size(c->rows[i].title, w, s.h_title);
		if (lv_obj_get_width(c->rows[i].sub) != w || lv_obj_get_height(c->rows[i].sub) != s.h_sub)
			lv_obj_set_size(c->rows[i].sub, w, s.h_sub);
	}
}

static void show_bubble(const column_t *c)
{
	lv_label_set_text(s.bubble_lbl, c->let[cur_letter(c)]);
	s.bubble_t = 1.0;
	set_hidden(s.bubble, false);
	lv_obj_set_style_opa(s.bubble, LV_OPA_COVER, 0);
}

static void hide_row(row_t *r)
{
	set_hidden(r->icon, true);
	set_hidden(r->img, true);
	set_hidden(r->title, true);
	set_hidden(r->sub, true);
}

/* строки создаются один раз на колонку; список любой длины — привязкой к видимым пунктам */
static void column_pool(column_t *c)
{
	if (!c->pool_ready) {
		for (int i = 0; i < POOL; i++) {
			row_t *r = &c->rows[i];
			r->img = lv_image_create(c->cont);   /* под иконкой: у играющего трека значок — поверх обложки */
			lv_obj_set_size(r->img, ROW_COVER, ROW_COVER);
			r->icon = label(c->cont, &lv_font_montserrat_16, "");
			lv_obj_set_size(r->icon, 40, 30);
			lv_obj_set_style_text_align(r->icon, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_set_style_transform_pivot_x(r->icon, 20, 0);
			lv_obj_set_style_transform_pivot_y(r->icon, 15, 0);
			r->title = label(c->cont, s.m_item, "");
			lv_obj_set_size(r->title, SCR_W - CAT_X - TEXT_DX - 6, s.h_title);
			lv_label_set_long_mode(r->title, LV_LABEL_LONG_MODE_DOTS);
			r->sub = label(c->cont, s.m_sub, "");
			lv_obj_set_size(r->sub, SCR_W - CAT_X - TEXT_DX - 6, s.h_sub);
			lv_label_set_long_mode(r->sub, LV_LABEL_LONG_MODE_DOTS);
		}
		c->pool_ready = 1;
	}
}

static void column_fill(column_t *c, const node_t *nodes, int count)
{
	column_pool(c);
	for (int i = 0; i < POOL; i++) {
		c->rows[i].idx = -1;
		hide_row(&c->rows[i]);
	}
	c->nodes = nodes;
	c->count = count;
	c->sel = 0;
	c->pos = 0;
	c->grid = 0;
	c->big = 0;
	c->rpos = 0;
	column_index(c);
	column_widths(c);
}

/* иконка строки: в сетке у альбома без обложки — пустая обложка (серый квадрат с серой нотой), в списке — значок */
static void grid_tile(row_t *r, const node_t *n, int size)
{
	int w = n ? size : 40, h = n ? size : 30;
	if (lv_obj_get_width(r->icon) != w || lv_obj_get_height(r->icon) != h) {
		lv_obj_set_size(r->icon, w, h);
		lv_obj_set_style_transform_pivot_x(r->icon, w / 2, 0);
		lv_obj_set_style_transform_pivot_y(r->icon, h / 2, 0);
	}
	lv_label_set_text(r->icon, n ? LV_SYMBOL_AUDIO : r->icon_txt);
	lv_obj_set_style_text_color(r->icon, n ? lv_color_hex(EMPTY_FG) : lv_color_white(), 0);
	if (n)
		lv_obj_set_style_bg_color(r->icon, lv_color_hex(EMPTY_BG), 0);
	else
		lv_obj_set_style_bg_opa(r->icon, LV_OPA_TRANSP, 0);
}

/* у играющего трека в списке вместо ноты — «играет» или «пауза» */
static const char *row_icon(const node_t *n)
{
	if (n->action != A_TRACK || s.track < 0 || n->track != s.track)
		return n->icon;
	return s.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE;
}

/* «год · N треков» альбома (и его исполнитель — в artist), для списка с обложками */
static void album_extra(const node_t *n, const char **artist, char *b, size_t cap)
{
	int a = 0;
	while (a < s.lib->nalbums && s.album_nodes[a] != n->kids)
		a++;
	int k = n->nkids, m10 = k % 10, m100 = k % 100, year = a < s.lib->nalbums ? s.lib->albums[a].year : 0;
	*artist = a < s.lib->nalbums ? tx(s.lib->albums[a].artist) : node_sub(n);
	const char *w = s.lang == LANG_EN ? (k == 1 ? "track" : "tracks") : s.lang == LANG_ES ? (k == 1 ? "pista" : "pistas") :
			m10 == 1 && m100 != 11 ? "трек" : m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14) ? "трека" : "треков";
	if (year)
		snprintf(b, cap, "%d · %d %s", year, k, w);
	else
		snprintf(b, cap, "%d %s", k, w);
}

/* шрифт, выравнивание и размер подписей — по виду колонки (пул строк общий для списков и сеток) */
static void row_style(const column_t *c, row_t *r)
{
	const lv_font_t *f = c->grid ? s.m_sub : s.m_item;
	lv_text_align_t al = c->grid ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT;
	int w = c->grid ? GRID_CELL - 6 : SCR_W - CAT_X - TEXT_DX - 6 - (c->nlet ? IDX_W : 0) - (c->big ? BIG_COVER - ROW_COVER : 0);
	int th = c->grid ? s.h_sub + 1 : s.h_title, sh = c->big ? 2 * s.h_sub : s.h_sub;
	if (lv_obj_get_style_text_font(r->title, 0) != f)
		lv_obj_set_style_text_font(r->title, f, 0);
	if (lv_obj_get_style_text_align(r->title, 0) != al) {
		lv_obj_set_style_text_align(r->title, al, 0);
		lv_obj_set_style_text_align(r->sub, al, 0);
	}
	if (lv_obj_get_width(r->title) != w || lv_obj_get_height(r->title) != th)
		lv_obj_set_size(r->title, w, th);
	if (lv_obj_get_width(r->sub) != w || lv_obj_get_height(r->sub) != sh)
		lv_obj_set_size(r->sub, w, sh);
}

static void bind_row(column_t *c, row_t *r, int i)
{
	const node_t *n = &c->nodes[i];
	r->idx = i;
	r->icon_txt = row_icon(n);
	row_style(c, r);
	lv_label_set_text(r->icon, r->icon_txt);
	lv_label_set_text(r->title, tx(n->title));
	if (c->big && n->kids) {
		const char *artist;
		char extra[64];
		album_extra(n, &artist, extra, sizeof(extra));
		lv_label_set_text_fmt(r->sub, "%s\n%s", artist, extra);
	} else {
		lv_label_set_text(r->sub, node_sub(n));
	}
	int cs = c->grid ? GRID_COVER : c->big ? BIG_COVER : ROW_COVER;
	grid_tile(r, c->grid ? n : NULL, GRID_COVER);
	if (lv_obj_get_width(r->img) != cs)
		lv_obj_set_size(r->img, cs, cs);
	if (lv_image_get_scale(r->img) != 256)
		lv_image_set_scale(r->img, 256);
	const lv_image_dsc_t *d = n->cover ? cover_get(n->cover, cs) : NULL;
	lv_image_set_src(r->img, d);
}

static void hide_column(column_t *c)
{
	for (int i = 0; i < POOL && c->pool_ready; i++)
		hide_row(&c->rows[i]);
}

static void refresh_subs(void)
{
	for (int d = 0; d <= s.depth; d++) {
		column_t *c = col_at(d);
		for (int i = 0; i < POOL; i++)
			if (c->rows[i].idx >= 0 && c->rows[i].idx < c->count)
				lv_label_set_text(c->rows[i].sub, node_sub(&c->nodes[c->rows[i].idx]));
	}
}

/* ---------- раскладка ---------- */

/* x колонки по её удалению от текущего уровня: родители собираются слева в цепочку иконок */
static double trail_x(double rel)
{
	if (rel > 0)
		return CAT_X + rel * 60;
	if (rel >= -1)
		return CAT_X + rel * 48;
	return CAT_X - 48 + (rel + 1) * 30;
}

/* левый край сетки от x колонки (центра иконок списка) */
static double grid_x0(double x)
{
	return x - 30;
}

/* ряд сетки — обложка и название под ней; у выбранного под названием ещё строка (исполнитель или год), ряды ниже
 * раздвигаются на неё плавно, по мере подхода выбора */
static int grid_row_h(void)
{
	return GRID_COVER + 8 + s.h_sub;
}

/* верхний видимый ряд (дробный): сетка — на всю высоту, выбранный ряд — вторым сверху, у краёв упирается */
static double grid_top(const column_t *c)
{
	int rows = (c->count + GRID_COLS - 1) / GRID_COLS;
	double vis = (double)(SCR_H - MINI_H - GRID_TOP - s.h_sub) / grid_row_h(), max = rows - vis;
	double top = c->rpos - 1;
	return top > max ? (max > 0 ? max : 0) : top < 0 ? 0 : top;
}

/* центр обложки ряда row по вертикали */
static double grid_cy(const column_t *c, double top, int row)
{
	return GRID_TOP + GRID_COVER / 2.0 + (row - top) * grid_row_h() + s.h_sub * clamp01(row - c->rpos);
}

/* сетка обложек на всю высоту, под каждой — название. У родителя (tk → 0) остаётся только выбранная обложка:
 * уменьшается и уезжает в цепочку, как иконка списка */
static void layout_grid(column_t *c, double x, double fade, double tk, int active)
{
	double top = grid_top(c);
	int r0 = (int)floor(top);
	int lo = (r0 - 1) * GRID_COLS, hi = (r0 + 5) * GRID_COLS - 1;
	lo = lo < 0 ? 0 : lo;
	hi = hi >= c->count ? c->count - 1 : hi;
	for (int k = 0; k < POOL; k++)
		if (c->rows[k].idx < lo || c->rows[k].idx > hi) {
			c->rows[k].idx = -1;
			hide_row(&c->rows[k]);
		}
	int lim = SCR_H - MINI_H, tw = GRID_CELL - 6;
	for (int i = lo; i <= hi; i++) {
		row_t *r = &c->rows[i % POOL];
		const node_t *n = &c->nodes[i];
		if (r->idx != i)
			bind_row(c, r, i);
		int sel = i == c->sel;
		double cx = grid_x0(x) + GRID_CELL / 2.0 + (i % GRID_COLS) * GRID_CELL;
		double cy = grid_cy(c, top, i / GRID_COLS);
		/* у краёв ряды появляются и уходят плавно; низ ячейки — с подписью */
		double t0 = cy - GRID_COVER / 2.0, t1 = cy + GRID_COVER / 2.0 + 4 + s.h_sub * (sel ? 2 : 1);
		double edge = clamp01((t0 - (GRID_TOP - 14)) / 12.0) * clamp01((lim + 14 - t1) / 12.0);
		double k = (sel && active ? 1.0 : active ? 0.62 : 0.45) * fade;
		if (sel) {   /* к звену цепочки */
			cx = x + (cx - x) * tk;
			cy = DEEP_SEL_Y + (cy - DEEP_SEL_Y) * tk;
			k = tk * k * edge + (1 - tk) * 0.5 * fade;
		} else {
			k *= tk * edge;
		}
		if (k < 0.02 || t1 < TOP_H - 10 || t0 > lim) {
			hide_row(r);
			continue;
		}
		/* обложка: в сетке — GRID_COVER, в цепочке — ROW_COVER; масштаб — только на время сворачивания */
		int cs = tk > 0.02 ? GRID_COVER : ROW_COVER;
		const lv_image_dsc_t *cd = n->cover ? cover_get(n->cover, cs) : NULL;
		if (lv_obj_get_width(r->img) != cs)
			lv_obj_set_size(r->img, cs, cs);
		if ((const void *)cd != lv_image_get_src(r->img))
			lv_image_set_src(r->img, cd);
		/* масштаб (трансформация через слой) — только у выбранной, уходящей в цепочку; остальные просто гаснут */
		int sc = sel && cs == GRID_COVER && tk < 0.999 ? (int)(256 * (ROW_COVER + (GRID_COVER - ROW_COVER) * tk) / GRID_COVER) : 256;
		if (lv_image_get_scale(r->img) != sc)
			lv_image_set_scale(r->img, sc);
		set_hidden(r->img, !cd);
		set_hidden(r->icon, cd != NULL);
		if (cd) {
			set_pos(r->img, (int)cx - cs / 2, (int)cy - cs / 2);
			int io = (int)(255 * fmin(1.0, k * 1.1));
			if (lv_obj_get_style_image_opa(r->img, 0) != io)
				lv_obj_set_style_image_opa(r->img, (lv_opa_t)io, 0);
		} else {
			/* квадрат цвета альбома в сетке; в цепочке — обычный значок */
			if ((tk > 0.5) != (lv_obj_get_width(r->icon) == GRID_COVER))   /* стили — только при смене вида */
				grid_tile(r, tk > 0.5 ? n : NULL, GRID_COVER);
			int w = lv_obj_get_width(r->icon), h = lv_obj_get_height(r->icon);
			set_pos(r->icon, (int)cx - w / 2, (int)cy - h / 2);
			set_icon_px(r->icon, tk > 0.5 ? 32 : 14);
			set_opa(r->icon, (int)(255 * k));
			/* прозрачность фона отдельно от текста: opa всего объекта рисовалась бы через слой */
			int bo = tk > 0.5 ? (int)(255 * fmin(1.0, k * 1.1)) : 0;
			if (lv_obj_get_style_bg_opa(r->icon, 0) != bo)
				lv_obj_set_style_bg_opa(r->icon, (lv_opa_t)bo, 0);
		}
		/* подпись под каждой обложкой: название, у выбранного — и исполнитель (год) */
		int show_t = tk > 0.5;
		set_hidden(r->title, !show_t);
		set_hidden(r->sub, !(show_t && sel));
		if (show_t) {
			int ty = (int)(cy + GRID_COVER / 2.0 + 3);
			double kt = k * (tk - 0.5) * 2;
			set_pos(r->title, (int)cx - tw / 2, ty);
			set_pos(r->sub, (int)cx - tw / 2, ty + s.h_sub);
			set_opa(r->title, (int)(255 * fmin(1.0, kt * 1.1)));
			set_opa(r->sub, (int)(190 * kt));
		}
		if (sel && active) {
			s.glow_x = (int)cx;
			s.glow_y = (int)cy;
			s.glow_k = k;
			s.glow_sz = tk > 0.5 ? GRID_COVER + GLOW_PAD : GLOW;
		}
	}
}

/* ячейка сетки под пальцем или −1 */
static int grid_hit(const column_t *c, lv_point_t p)
{
	double x0 = grid_x0(CAT_X), top = grid_top(c);
	int col = (int)floor((p.x - x0) / GRID_CELL);
	int row = (int)floor(top + (p.y - GRID_TOP + 5) / grid_row_h());
	if (row > c->rpos)   /* ниже выбранного ряды раздвинуты на строку подписи */
		row = (int)floor(top + (p.y - GRID_TOP + 5 - s.h_sub) / grid_row_h());
	if (col < 0 || col >= GRID_COLS || p.y < GRID_TOP - 5 || p.y > SCR_H - MINI_H)
		return -1;
	int i = row * GRID_COLS + col;
	return i >= 0 && i < c->count ? i : -1;
}

/* fade — общая прозрачность колонки, tk — видимость заголовков и невыбранных строк (0 у родителей) */
static void layout_column(column_t *c, double x, double fade, double tk, int active, int show_sel, int deep)
{
	if (c->grid) {
		layout_grid(c, x, fade, tk, active);
		return;
	}
	int lo = (int)floor(c->pos) - 4, hi = (int)floor(c->pos) + 9;
	lo = lo < 0 ? 0 : lo;
	hi = hi >= c->count ? c->count - 1 : hi;
	for (int k = 0; k < POOL; k++)   /* строки, привязанные к пунктам вне окна, свободны */
		if (c->rows[k].idx < lo || c->rows[k].idx > hi) {
			c->rows[k].idx = -1;
			hide_row(&c->rows[k]);
		}
	for (int i = lo; i <= hi; i++) {
		row_t *r = &c->rows[i % POOL];
		if (r->idx != i)
			bind_row(c, r, i);
		else if (c->nodes[i].cover) {   /* опрос каждый кадр держит обложку в кэше и подхватывает готовую */
			const lv_image_dsc_t *cd = cover_get(c->nodes[i].cover, c->big ? BIG_COVER : ROW_COVER);
			if ((const void *)cd != lv_image_get_src(r->img))
				lv_image_set_src(r->img, cd);
		}
		if (row_icon(&c->nodes[i]) != r->icon_txt) {   /* сменился играющий трек или пауза */
			r->icon_txt = row_icon(&c->nodes[i]);
			lv_label_set_text(r->icon, r->icon_txt);
		}
		double d = i - c->pos, rh = c->big ? BIG_ROW : ROW;
		double y;
		if (deep)
			y = DEEP_SEL_Y + d * rh;
		else if (d >= 0)
			y = ITEM_SEL_Y + d * rh;
		else if (d > -1)
			y = ITEM_ABOVE_Y + (d + 1) * (ITEM_SEL_Y - ITEM_ABOVE_Y);
		else
			y = ITEM_ABOVE_Y + (d + 1) * rh;
		double near = show_sel ? fmax(0.0, 1.0 - fabs(d)) : 0;
		double base = d < -0.5 ? 0.45 : 0.62;
		double k = (active ? base + (1.0 - base) * near : base * 0.75) * fade;
		int sel = show_sel && near > 0.5;
		/* у родителя остаётся только иконка выбранного пункта — звено цепочки */
		double k_icon = sel ? tk * k + (1 - tk) * 0.5 * fade : k * tk;
		double k_text = k * tk;
		int yy = (int)y;
		/* у краёв списка строки появляются и уходят плавно, а не за один кадр */
		/* сверху строка уходит под чёрную верхнюю панель — гаснет, только когда уже под ней; снизу так же уходит под
		 * мини-плеер */
		int top = deep ? TOP_H - (int)(rh * 0.3) : TOP_H - 14, bot = SCR_H - MINI_H + (int)(rh * 0.45);
		double edge = clamp01((yy - top) / (rh * 0.7)) * clamp01((bot - yy) / (rh * 0.6));
		/* на уровне категорий пункт, уезжающий вверх, пролетает сквозь иконку и название категории — там гаснет */
		if (!deep && d < 0)
			edge *= clamp01(fabs((double)(yy - CAT_Y)) / 46.0 - 0.3);
		k_icon *= edge;
		k_text *= edge;
		if (yy < top || yy > bot || k_icon < 0.02) {
			hide_row(r);
			continue;
		}
		/* обложка вместо иконки, пока колонка не свернулась в цепочку */
		int show_img = lv_image_get_src(r->img) && tk > 0.5;
		/* играющий трек: значок «играет» / «пауза» вместо ноты или обложки */
		if (r->icon_txt != c->nodes[i].icon)
			show_img = 0;
		set_hidden(r->img, !show_img);
		set_hidden(r->icon, show_img);
		/* у списка с обложками обложка крупнее: левый край — тот же, текст — правее на разницу */
		int cs = c->big && tk > 0.5 ? BIG_COVER : ROW_COVER, cdx = (cs - ROW_COVER) / 2;
		if (show_img) {
			if (lv_obj_get_width(r->img) != cs)
				lv_obj_set_size(r->img, cs, cs);
			set_pos(r->img, (int)x + cdx - cs / 2, yy - cs / 2);
			int io = (int)(255 * fmin(1.0, k_icon * 1.15));
			if (lv_obj_get_style_image_opa(r->img, 0) != io)
				lv_obj_set_style_image_opa(r->img, (lv_opa_t)io, 0);
		}
		set_hidden(r->title, k_text < 0.02);
		set_hidden(r->sub, !(sel || c->big) || k_text < 0.02);
		/* список с обложками: у альбома без картинки — пустая обложка того же размера */
		int tile = c->big && tk > 0.5 && !show_img && r->icon_txt == c->nodes[i].icon;
		if (tile != (lv_obj_get_width(r->icon) == BIG_COVER))
			grid_tile(r, tile ? &c->nodes[i] : NULL, BIG_COVER);
		if (tile) {
			set_pos(r->icon, (int)x + cdx - BIG_COVER / 2, yy - BIG_COVER / 2);
			int bo = (int)(255 * fmin(1.0, k_icon * 1.15));
			if (lv_obj_get_style_bg_opa(r->icon, 0) != bo)
				lv_obj_set_style_bg_opa(r->icon, (lv_opa_t)bo, 0);
		} else {
			set_pos(r->icon, (int)x + cdx - 20, yy - 15);
		}
		int has_sub = lv_label_get_text(r->sub)[0] != 0;   /* без второй строки название — по центру строки */
		if (c->big) {   /* три строки текста по высоте обложки */
			int ty = has_sub ? yy - (s.h_title + 2 * s.h_sub) / 2 : yy + s.dy_title;
			set_pos(r->title, (int)x + TEXT_DX + 2 * cdx, ty);
			set_pos(r->sub, (int)x + TEXT_DX + 2 * cdx, ty + s.h_title);
		} else {
			set_pos(r->title, (int)x + TEXT_DX, yy + (sel && has_sub ? s.dy_title_sel : s.dy_title));
			set_pos(r->sub, (int)x + TEXT_DX, yy + s.dy_sub);
		}
		/* выбранный — icon_sel, остальные — icon, звенья цепочки — 14 */
		set_icon_px(r->icon, tile ? 24 : 14 + (sz->icon - 14 + (sz->icon_sel - sz->icon) * near) * tk);
		if (sel && active) {
			s.glow_x = (int)x + (c->big && tk > 0.5 ? cdx : 0);
			s.glow_y = yy;
			s.glow_k = k_icon;
			s.glow_sz = c->big && tk > 0.5 ? BIG_COVER + GLOW_PAD : GLOW;
		}
		set_opa(r->icon, (int)(255 * k_icon));
		set_opa(r->title, (int)(255 * k_text));
		set_opa(r->sub, (int)(180 * k_text));
	}
}

static void layout_options(void)
{
	/* правый край рамки подбирают на глаз — меню не должно его закрывать: выезжает слева */
	int left = s.opt_item && s.opt_item->val == &s.edge_r;
	int x = left ? (int)(s.opt_w * (s.opt_t - 1)) : (int)(SCR_W - s.opt_w * s.opt_t);
	set_pos(s.opt_panel, x, 0);
	set_hidden(s.opt_panel, s.opt_t < 0.01);
	int sub = (int)(s.opt_sub * s.opt_w * 0.5);   /* вложенное меню: въезжает и проявляется */
	int tx = 24 + sub;   /* галочка выбранного — справа, текст ближе к краю; у тем — только образцы */
	/* с предпросмотром выбранное и есть значение: галочка стоит у подсветки и не обгоняет прокрутку */
	int preview = s.opt_item && s.opt_item->apply && !s.opt_item->commit;
	for (int i = 0; i < s.opt_count; i++) {
		double d = i - s.opt_pos;
		int y = (int)(SCR_H / 2 - 10 + d * OPT_ROW);   /* середина строчных и заглавных — по середине подсветки */
		double near = fmax(0.0, 1.0 - fabs(d));
		int off = y < -OPT_ROW || y > SCR_H;
		int mine = s.opt_item && !s.opt_item->commit && i == *s.opt_item->val;
		set_pos(s.opt_rows[i], tx, y);
		set_pos(s.opt_checks[i], s.opt_w - 34 + sub, preview ? SCR_H / 2 - 10 : y);
		set_opa(s.opt_rows[i], (int)(255 * (0.5 + 0.5 * near) * (1.0 - s.opt_sub)));
		set_opa(s.opt_checks[i], (int)(255 * (1.0 - s.opt_sub)));
		set_hidden(s.opt_rows[i], off || s.opt_sw[i]);
		set_hidden(s.opt_checks[i], !mine || (!preview && off));
		if (s.opt_sw[i]) {
			set_pos(s.opt_sw[i], 18 + sub, y - 3);
			set_hidden(s.opt_sw[i], off);
		}
	}
}

#define TILE_X(i) (40 + (i) % 4 * 80)
#define TILE_Y(i) (TOP_H + 34 + (i) / 4 * 82)
#define TILE_R 26
#define SLIDER_Y(i) (TOP_H + 206 + (i) * 44)
#define SLIDER_X 56
#define SLIDER_W 246

static int tile_on(int i)
{
	switch (i) {
	case TL_BT: return !s.bt_off;
	case TL_WIFI: return !s.wifi_off;
	case TL_EQ: return s.eq_on;
	case TL_SLEEP: return s.sleep_end != 0;
	case TL_SHUFFLE: return s.shuffle;
	case TL_REPEAT: return s.repeat != 0;
	case TL_TOUCH: return !s.touch_off;
	default: return 0;
	}
}

static const char *tile_label(int i)
{
	static char left[24];
	if (i == TL_SLEEP && s.sleep_end) {
		int sec = (int)((int32_t)(s.sleep_end - lv_tick_get()) / 1000);
		sec = sec < 0 ? 0 : sec;
		snprintf(left, sizeof(left), "%d:%02d", sec / 60, sec % 60);
		return left;
	}
	if (i == TL_REPEAT)
		return tx(s.repeat == 2 ? "Один" : s.repeat == 1 ? "Все" : "Повтор");
	return tx(tiles[i].name);
}

static void layout_shade(void)
{
	set_hidden(s.sh_panel, s.sh_t < 0.01);
	set_pos(s.sh_panel, 0, (int)(-SHADE_H + SHADE_H * s.sh_t));
	if (s.sh_t < 0.01)
		return;
	for (int i = 0; i < NTILE; i++) {
		int on = tile_on(i);
		lv_color_t bg = on ? lv_color_hex(ACCENT) : lv_color_white();
		if (!lv_color_eq(lv_obj_get_style_bg_color(s.sh_bg[i], 0), bg)) {
			lv_obj_set_style_bg_color(s.sh_bg[i], bg, 0);
			lv_obj_set_style_bg_opa(s.sh_bg[i], on ? 235 : 34, 0);
		}
		set_opa(s.sh_icon[i], on ? 255 : 170);
		set_opa(s.sh_label[i], on ? 235 : 150);
		const char *t = tile_label(i);
		if (strcmp(lv_label_get_text(s.sh_label[i]), t))
			lv_label_set_text(s.sh_label[i], t);
	}
}

/* полоса указателя: по центру области списка, шаг букв не больше 16 px */
static void index_geom(const column_t *c, int *y0, int *step)
{
	int top = TOP_H + 14, bot = SCR_H - MINI_H - 10;
	*step = (bot - top) / c->nlet < 16 ? (bot - top) / c->nlet : 16;
	*y0 = (top + bot) / 2 - *step * c->nlet / 2;
}

static column_t *index_column(void)
{
	column_t *c = s.at_cats ? NULL : cur_col();
	return c && c->nlet && player_progress() == 0 && !s.opt_open && s.depth_t == s.depth ? c : NULL;
}

static int index_hit(lv_point_t p)
{
	column_t *c = index_column();
	if (!c || p.x < SCR_W - IDX_W - 12)
		return 0;
	int y0, step;
	index_geom(c, &y0, &step);
	return p.y >= y0 - 12 && p.y <= y0 + step * c->nlet + 12;
}

static void index_touch(int y)
{
	column_t *c = index_column();
	if (!c)
		return;
	int y0, step;
	index_geom(c, &y0, &step);
	int k = (y - y0) / step;
	k = k < 0 ? 0 : k >= c->nlet ? c->nlet - 1 : k;
	if (c->sel != c->let_at[k] || s.bubble_t < 0.5) {
		c->sel = c->let_at[k];
		c->pos = c->sel;   /* сразу, без прокрутки через весь список */
		c->rpos = c->sel / GRID_COLS;
		show_bubble(c);
	}
}

static void index_draw(lv_event_t *e)
{
	column_t *c = index_column();
	if (!c)
		return;
	lv_layer_t *l = lv_event_get_layer(e);
	int y0, step, cur = cur_letter(c);
	index_geom(c, &y0, &step);
	lv_draw_label_dsc_t d;
	lv_draw_label_dsc_init(&d);
	d.font = s.f_bat;
	d.align = LV_TEXT_ALIGN_CENTER;
	for (int k = 0; k < c->nlet; k++) {
		d.color = k == cur ? lv_color_hex(ACCENT) : lv_color_white();
		d.opa = k == cur ? 255 : 120;
		d.text = c->let[k];
		int y = y0 + k * step + (step - lv_font_get_line_height(s.f_bat)) / 2;
		lv_area_t a = { SCR_W - IDX_W, y, SCR_W - 1, y + step };
		lv_draw_label(l, &d, &a);
	}
}

static void layout_index(void)
{
	static const column_t *last_c;
	static int last_k = -1, last_n;
	column_t *c = index_column();
	int k = c ? cur_letter(c) : -1, n = c ? c->nlet : 0;
	if (c != last_c || k != last_k || n != last_n) {
		last_c = c;
		last_k = k;
		last_n = n;
		set_hidden(s.idx_strip, !c);
		lv_obj_invalidate(s.idx_strip);
	}
}

/* колонка без видимых строк — скрыть контейнер целиком: иначе каждая перерисовка любой области обходит все его
 * строки (у LVGL цена области — обход дерева объектов, а колонок с пулами строк до десятка) */
static void column_cont_vis(column_t *c)
{
	int vis = 0;
	for (int i = 0; i < POOL && c->pool_ready && !vis; i++) {
		const row_t *r = &c->rows[i];
		vis = !lv_obj_is_hidden(r->icon) || !lv_obj_is_hidden(r->img) || !lv_obj_is_hidden(r->title) ||
		      !lv_obj_is_hidden(r->sub);
	}
	set_hidden(c->cont, !vis);
}

static void layout(void)
{
	double dt_ = s.depth_t, ft = s.focus_t, deep = clamp01(dt_);
	double cx = trail_x(-dt_);
	double ui = 1.0 - player_progress();   /* XMB гаснет, пока раскрывается плеер */
	s.glow_k = 0;
	s.glow_sz = GLOW;
	for (int c = 0; c < NCAT; c++) {
		double d = c - s.cat_pos;
		double near = fmax(0.0, 1.0 - fabs(d));
		double k = (0.5 + 0.5 * near) * (d < -0.5 ? 0.85 : 1.0);
		k *= near > 0.5 ? 1.0 - 0.55 * deep : 1.0 - deep;   /* соседи гаснут, выбранная остаётся крошкой */
		set_pos(s.cat_icons[c], (int)(cx + d * CAT_STEP) - 28, CAT_Y - 22);
		set_icon_px(s.cat_icons[c], sz->cat_icon + (sz->cat_icon_sel - sz->cat_icon) * near * (1.0 - deep));
		set_opa(s.cat_icons[c], (int)(255 * k * (0.75 + 0.25 * ft) * ui));
		set_hidden(s.cat_icons[c], k < 0.02);
	}
	set_opa(s.cat_title, (int)(255 * (1.0 - deep) * (0.65 + 0.35 * ft) * ui));

	for (int i = 0; i < MAX_DEPTH; i++) {
		column_t *c = col_at(i);
		if (!c->count)
			continue;
		double rel = i - dt_;
		if (rel >= 1 || (i > s.depth && rel <= 0)) {
			hide_column(c);
			continue;
		}
		double fade = (rel > 0 ? 1.0 - rel : 1.0) * ui;
		double tk = rel < 0 ? clamp01(1.0 + rel * 2) : 1.0;
		if (i == 0) {
			/* на уровне категорий — тусклое превью без выделения; при смене категории въезжает сбоку */
			fade *= (1.0 - 0.45 * ft) * (1.0 - fabs(s.slide));
			layout_column(c, trail_x(rel) + s.slide * 40, fade, tk, !s.at_cats && s.depth == 0, !s.at_cats, 0);
		} else {
			layout_column(c, trail_x(rel), fade, tk, i == s.depth, 1, 1);
		}
	}
	/* свечение под выбранным: в ряду категорий — под категорией, иначе — под пунктом */
	if (s.at_cats) {
		s.glow_x = (int)cx;
		s.glow_y = CAT_Y;
		s.glow_k = 1.0 - deep;
	}
	/* под обложкой свечение крупнее: видно его край вокруг картинки */
	static int glow_cur = GLOW;
	if (glow_cur != s.glow_sz) {
		glow_cur = s.glow_sz;
		lv_image_set_src(s.glow, bg_glow(s.glow_sz));
	}
	set_pos(s.glow, s.glow_x - s.glow_sz / 2, s.glow_y - s.glow_sz / 2);
	int gk = (int)((s.glow_sz > GLOW ? 150 : 90) * clamp01(s.glow_k) * (1.0 - fabs(s.slide)) * ui);
	if (lv_obj_get_style_image_opa(s.glow, 0) != gk)
		lv_obj_set_style_image_opa(s.glow, (lv_opa_t)gk, 0);
	set_hidden(s.glow, gk < 4);
	layout_index();
	for (int c = 0; c < NCAT; c++)
		column_cont_vis(&s.cat_col[c]);
	for (int d = 1; d < MAX_DEPTH; d++)
		column_cont_vis(&s.stack[d]);
}

/* ---------- верхняя панель, мини-плеер ---------- */

/* батарея как в Android 16: таблетка, процент внутри, заливка — уровень; зелёная при зарядке, красная при ≤15% */
static void battery_draw(lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(s.tb_bat, &a);
	int lvl = s.bat < 0 ? 0 : s.bat > 100 ? 100 : s.bat;
	lv_color_t fill = s.bat_chg ? lv_color_hex(0x5fd068) : lvl <= 15 ? lv_color_hex(0xff5a4e) : lv_color_white();
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.radius = LV_RADIUS_CIRCLE;
	d.bg_color = lv_color_white();
	d.bg_opa = 70;
	lv_draw_rect(l, &d, &a);
	lv_area_t f = a;
	f.x2 = a.x1 + (lv_area_get_width(&a) - 1) * lvl / 100;
	d.bg_color = fill;
	d.bg_opa = LV_OPA_COVER;
	lv_draw_rect(l, &d, &f);
	char txt[8];
	snprintf(txt, sizeof(txt), "%d", lvl);
	lv_draw_label_dsc_t t;
	lv_draw_label_dsc_init(&t);
	t.font = s.f_bat;
	t.color = lv_color_hex(0x101216);
	t.align = LV_TEXT_ALIGN_CENTER;
	t.text = txt;
	t.text_local = 1;   /* отрисовка откладывается: строку со стека LVGL должен скопировать */
	/* по центру — сами цифры, а не строка шрифта и не рамка глифа (у ttf в ней пустые строки снизу):
	 * высота цифр Noto Sans — 0.714 кегля, верх цифры — верх рамки глифа */
	lv_font_glyph_dsc_t g;
	int mid = (a.y1 + a.y2 + 1) / 2, y1 = a.y1, ink = (int)(BAT_FONT_PX * 0.714f + 0.5f);
	if (lv_font_get_glyph_dsc(s.f_bat, &g, '0', 0))
		y1 = mid - ink / 2 - (lv_font_get_line_height(s.f_bat) - s.f_bat->base_line) + g.box_h + g.ofs_y;
	lv_area_t ta = { a.x1, y1, a.x2, a.y2 + 8 };
	lv_draw_label(l, &t, &ta);
}

static void update_topbar(void)
{
	static char last_icons[64];
	static int last_min = -1, last_bat = -2, last_chg = -1;
	char buf[64];
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	if (tm->tm_min != last_min) {
		last_min = tm->tm_min;
		lv_label_set_text_fmt(s.tb_left, "%02d:%02d", tm->tm_hour, tm->tm_min);
	}
	snprintf(buf, sizeof(buf), "%s%s%s%s", s.shuffle ? LV_SYMBOL_SHUFFLE "  " : "", s.repeat ? LV_SYMBOL_LOOP "  " : "",
		 s.bt_off ? "" : LV_SYMBOL_BLUETOOTH "  ", s.wifi_off ? "" : LV_SYMBOL_WIFI);
	if (strcmp(buf, last_icons)) {
		strcpy(last_icons, buf);
		lv_label_set_text(s.tb_right, buf);
	}
	/* процент батареи — плавно: датчик скачет на нагрузке, показываем не больше ±1 за 20 с */
	static uint32_t bat_step_ms;
	int real = platform_battery(&s.bat_chg);
	uint32_t tnow = lv_tick_get();
	if (s.bat <= 0 || real < 0)
		s.bat = real;
	else if (real != s.bat && tnow - bat_step_ms > 20000) {
		s.bat += real > s.bat ? 1 : -1;
		bat_step_ms = tnow;
	}
	if (s.bat != last_bat || s.bat_chg != last_chg) {
		last_bat = s.bat;
		last_chg = s.bat_chg;
		lv_obj_invalidate(s.tb_bat);
	}
}

/* ---------- библиотека → пункты меню ---------- */

static struct {
	volatile int progress, done;
	library_t *result;
	char root[256], cache[300];
} scan;

static void *scan_thread(void *arg)
{
	(void)arg;
	scan.result = lib_scan(scan.root, scan.cache, &scan.progress);
	__sync_synchronize();
	scan.done = 1;
	return NULL;
}

/* корень музыки: XMB_MUSIC, на плеере — SD-карта; кэш тегов — в .xmb на ней же */
static void library_start(void)
{
	const char *root = music_root();
	if (!root) {
		cover_init(NULL);
		scan.result = calloc(1, sizeof(library_t));
		scan.done = 1;
		return;
	}
	snprintf(scan.root, sizeof(scan.root), "%s", root);
	snprintf(scan.cache, sizeof(scan.cache), "%s/.xmb", root);
	mkdir(scan.cache, 0755);
	snprintf(scan.cache, sizeof(scan.cache), "%s/.xmb/covers", root);
	cover_init(scan.cache);
	snprintf(scan.cache, sizeof(scan.cache), "%s/.xmb/library.tsv", root);
	pthread_t t;
	pthread_create(&t, NULL, scan_thread, NULL);
}

static node_t *track_nodes(const int *idx, int n)
{
	node_t *v = calloc(n ? n : 1, sizeof(node_t));
	for (int i = 0; i < n; i++) {
		v[i].icon = LV_SYMBOL_AUDIO;
		v[i].title = s.lib->tracks[idx[i]].title;
		v[i].action = A_TRACK;
		v[i].track = idx[i];
	}
	return v;
}

static void album_node(node_t *v, int a, const char *sub)
{
	const lib_album_t *al = &s.lib->albums[a];
	v->icon = LV_SYMBOL_IMAGE;
	v->title = al->title;
	v->sub = sub;
	v->kids = s.album_nodes[a];
	v->nkids = al->ntracks;
	v->cover = al->cover;
}

/* папка: сначала подпапки, потом треки; обложка папки — альбома её первого трека */
static node_t *dir_nodes(const lib_dir_t *d, int *n)
{
	*n = d->ndirs + d->ntracks;
	node_t *v = calloc(*n ? *n : 1, sizeof(node_t));
	for (int i = 0; i < d->ndirs; i++) {
		const lib_dir_t *sd = &d->dirs[i];
		int k;
		v[i].icon = LV_SYMBOL_DIRECTORY;
		v[i].title = sd->name;
		v[i].key = sd->name;
		v[i].kids = dir_nodes(sd, &k);
		v[i].nkids = k;
		if (!k)
			v[i].kids = NULL;
		if (sd->ntracks)
			v[i].cover = s.lib->albums[s.lib->tracks[sd->tracks[0]].aidx].cover;
	}
	node_t *t = track_nodes(d->tracks, d->ntracks);
	memcpy(v + d->ndirs, t, sizeof(node_t) * d->ntracks);
	free(t);
	return v;
}

static int album_cmp(const void *a, const void *b)
{
	const lib_album_t *x = &s.lib->albums[*(const int *)a], *y = &s.lib->albums[*(const int *)b];
	if (x->podcast != y->podcast)
		return x->podcast - y->podcast;
	int c = lib_strcmp(x->artist, y->artist);
	if (c)
		return c;
	if (x->year != y->year)
		return x->year - y->year;
	return lib_strcmp(x->title, y->title);
}

static void library_build(void)
{
	library_t *L = s.lib;
	static node_t empty[1];
	int na = L->nalbums;
	s.album_order = malloc(sizeof(int) * (na ? na : 1));
	s.album_pos = malloc(sizeof(int) * (na ? na : 1));
	for (int a = 0; a < na; a++)
		s.album_order[a] = a;
	qsort(s.album_order, na, sizeof(int), album_cmp);
	for (int a = 0; a < na; a++)
		s.album_pos[s.album_order[a]] = a;
	s.album_nodes = malloc(sizeof(node_t *) * (na ? na : 1));
	for (int a = 0; a < na; a++)
		s.album_nodes[a] = track_nodes(L->albums[a].tracks, L->albums[a].ntracks);
	nmusic = 0;
	while (nmusic < na && !L->albums[s.album_order[nmusic]].podcast)
		nmusic++;
	int npod = na - nmusic;
	pod_nodes = calloc(npod + 1, sizeof(node_t));
	pod_nodes[0] = (node_t){ .icon = LV_SYMBOL_REFRESH, .title = "Обновить", .sub = pod_upd_sub, .action = A_PODUPDATE };
	for (int i = 0; i < npod; i++)
		album_node(&pod_nodes[1 + i], s.album_order[nmusic + i], NULL);

	node_t *albums = calloc(na ? na : 1, sizeof(node_t));
	for (int i = 0; i < nmusic; i++) {   /* альбомы отсортированы по исполнителю — по нему и указатель */
		album_node(&albums[i], s.album_order[i], L->albums[s.album_order[i]].artist);
		albums[i].key = L->albums[s.album_order[i]].artist;
	}

	node_t *artists = calloc(L->nartists ? L->nartists : 1, sizeof(node_t));
	for (int i = 0; i < L->nartists; i++) {
		const lib_artist_t *ar = &L->artists[i];
		node_t *k = calloc(ar->nalbums ? ar->nalbums : 1, sizeof(node_t));
		for (int j = 0; j < ar->nalbums; j++) {
			const lib_album_t *al = &L->albums[ar->albums[j]];
			char *y = NULL;
			if (al->year) {
				y = malloc(8);
				snprintf(y, 8, "%d", al->year);
			}
			album_node(&k[j], ar->albums[j], y);
		}
		artists[i] = (node_t){ .icon = LV_SYMBOL_AUDIO, .title = ar->name, .kids = k, .nkids = ar->nalbums, .key = ar->name, .grid = 1 };
		if (ar->nalbums)
			artists[i].cover = L->albums[ar->albums[0]].cover;
	}

	node_t *all = track_nodes(L->by_title, L->nby_title);
	for (int i = 0; i < L->nby_title; i++) {   /* список из разных альбомов: у трека — обложка его альбома */
		all[i].key = L->albums[L->tracks[L->by_title[i]].aidx].artist;
		all[i].cover = L->albums[L->tracks[L->by_title[i]].aidx].cover;
	}
	if (L->ntracks) {
		music[MUSIC_QUEUE] = (node_t){ .icon = LV_SYMBOL_NEXT, .title = "Очередь", .sub = "пусто" };   /* q_changed */
		music[MUSIC_ALBUMS] = (node_t){ .icon = LV_SYMBOL_IMAGE, .title = "Альбомы", .kids = albums, .nkids = nmusic, .grid = 1 };
		music[MUSIC_ARTISTS] = (node_t){ .icon = LV_SYMBOL_AUDIO, .title = "Исполнители", .kids = artists, .nkids = L->nartists };
		music[MUSIC_ALL] = (node_t){ .icon = LV_SYMBOL_LIST, .title = "Все треки", .kids = all,
					     .nkids = L->nby_title };
		music[MUSIC_PODCASTS] = (node_t){ .icon = LV_SYMBOL_DOWNLOAD, .title = "Подкасты", .sub = pod_sub, .kids = pod_nodes,
						  .nkids = npod + 1 };
		pod_labels();
		music[MUSIC_SEARCH] = (node_t){ .icon = LV_SYMBOL_EYE_OPEN, .title = "Поиск", .sub = "исполнители, альбомы, треки",
						.action = A_SEARCH };
		lib_artists = artists;
		lib_albums = albums;
		cats[CAT_MUSIC].items = music;
		cats[CAT_MUSIC].count = MUSIC_COUNT;
		int n;
		cats[CAT_FOLDERS].items = dir_nodes(&L->root, &n);
		cats[CAT_FOLDERS].count = n;
	} else {
		empty[0] = (node_t){ .icon = LV_SYMBOL_WARNING, .title = "Музыка не найдена",
				     .sub = XMB_DEVICE ? "на карте нет FLAC, MP3 или WAV" : "укажите папку в XMB_MUSIC" };
		cats[CAT_MUSIC].items = cats[CAT_FOLDERS].items = empty;
		cats[CAT_MUSIC].count = cats[CAT_FOLDERS].count = 1;
	}
	column_fill(&s.cat_col[CAT_MUSIC], cats[CAT_MUSIC].items, cats[CAT_MUSIC].count);
	if (cats[CAT_MUSIC].items == music)
		s.cat_col[CAT_MUSIC].sel = s.cat_col[CAT_MUSIC].pos = MUSIC_ALBUMS;
	column_fill(&s.cat_col[CAT_FOLDERS], cats[CAT_FOLDERS].items, cats[CAT_FOLDERS].count);
	const char *root = music_root();
	snprintf(s.pl_dir, sizeof(s.pl_dir), "%s/Playlists", root ? root : ".");
	s.npl = pl_load_all(L, s.pl_dir, &s.pls);
	playlists_build();
	printf("xmb: библиотека — %d треков, %d альбомов, %d исполнителей\n", L->ntracks, na, L->nartists);
	last_load();
}

#define NAV_FILE "/tmp/xmb_nav"

/* где стоит курсор: категория и номера пунктов по уровням — после перезапуска раскрыть так же */
static void nav_save(void)
{
	FILE *f = fopen(NAV_FILE, "w");
	if (!f)
		return;
	fprintf(f, "%d %d", s.cat, s.depth);
	for (int d = 0; d <= s.depth; d++)
		fprintf(f, " %d", col_at(d)->sel);
	fprintf(f, "\n");
	fclose(f);
}

static void reveal(int cat, const int *path, int n);

/* early — при запуске, библиотеки ещё нет: настройки раскрываются сразу, остальные категории только выбираются,
 * курсор в них — после сканирования */
static void nav_restore(int early)
{
	FILE *f = fopen(NAV_FILE, "r");
	int cat, depth, path[MAX_DEPTH], n = 0;
	if (!f)
		return;
	if (fscanf(f, "%d %d", &cat, &depth) == 2)
		while (n <= depth && n < MAX_DEPTH && fscanf(f, "%d", &path[n]) == 1)
			n++;
	fclose(f);
	if (!n || cat < 0 || cat >= NCAT) {
		unlink(NAV_FILE);
		return;
	}
	if (early && cat != CAT_SETTINGS) {
		s.cat = cat;
		s.cat_pos = cat;
		return;
	}
	unlink(NAV_FILE);
	reveal(cat, path, n);
}

void xmb_restart_prepare(void)
{
	if (s.lib)
		nav_save();
}

static void library_poll(void)
{
	static int last = -1;
	if (s.lib)
		return;
	if (!scan.done) {
		if (scan.progress / 20 != last) {
			last = scan.progress / 20;
			snprintf(scan_sub, sizeof(scan_sub), tx("просмотрено файлов: %d"), scan.progress);
			refresh_subs();
		}
		return;
	}
	s.lib = scan.result ? scan.result : calloc(1, sizeof(library_t));
	library_build();
	nav_restore(0);
}

/* фоновая работа — в статус-баре вместо значков режимов: сверху что идёт, под ним полоса */
static void activity_tick(void)
{
	static uint32_t last, probe;
	uint32_t now = lv_tick_get();
	double dt = (now - last) / 1000.0;
	last = now;
	int kind = !s.lib ? 1 : pod_busy() ? 2 : 0;
	double frac = s.act_frac;
	char text[64] = "";
	if (kind == 1) {
		frac = -1;
		snprintf(text, sizeof(text), tx("Библиотека: просмотрено %d"), scan.progress);
	} else if (kind == 2) {
		if (now - probe > 500) {   /* размер файла на карте — не каждый кадр */
			probe = now;
			frac = pod_progress();
		}
		if (!pod_downloading())
			snprintf(text, sizeof(text), "%s", tx("Подкасты: проверка"));
		else if (frac >= 0)
			snprintf(text, sizeof(text), tx("Подкасты: загрузка %d%%"), (int)(frac * 100));
		else
			snprintf(text, sizeof(text), "%s", tx("Подкасты: загрузка"));
	}
	if (kind != s.act_kind) {
		s.act_kind = kind;
		set_hidden(s.tb_act, !kind);
		set_hidden(s.tb_right, kind != 0);   /* значки режимов и радио — на время работы прячутся */
	}
	if (!kind)
		return;
	if (strcmp(text, s.act_text)) {
		snprintf(s.act_text, sizeof(s.act_text), "%s", text);
		lv_obj_invalidate(s.tb_act);
	}
	if (frac < 0) {
		s.act_phase = fmod(s.act_phase + fmin(dt, 0.1) / 1.4, 1.0);
		lv_obj_invalidate(s.tb_act);
	} else if (fabs(frac - s.act_frac) > 0.004) {
		lv_obj_invalidate(s.tb_act);
	}
	s.act_frac = frac;
}

static void activity_draw(lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(lv_event_get_target(e), &a);
	lv_draw_label_dsc_t t;
	lv_draw_label_dsc_init(&t);
	t.font = s.f_bat;
	t.color = lv_color_white();
	t.align = LV_TEXT_ALIGN_CENTER;
	t.text = s.act_text;
	t.text_local = 1;
	lv_area_t ta = { a.x1, a.y1, a.x2, a.y1 + 15 };
	lv_draw_label(l, &t, &ta);
	int w = 120, x0 = (a.x1 + a.x2 - w) / 2, x1 = x0 + w, y = a.y2 - 2;
	lv_draw_rect_dsc_t r;
	lv_draw_rect_dsc_init(&r);
	r.radius = 2;
	r.bg_color = lv_color_white();
	r.bg_opa = 60;
	lv_area_t ba = { x0, y - 2, x1, y + 1 };
	lv_draw_rect(l, &r, &ba);
	r.bg_color = lv_color_hex(ACCENT);
	r.bg_opa = LV_OPA_COVER;
	if (s.act_frac >= 0) {
		ba.x2 = x0 + (int)(w * s.act_frac);
	} else {
		int seg = w / 3, sx = x0 - seg + (int)((w + seg) * s.act_phase);
		ba.x1 = LV_MAX(x0, sx);
		ba.x2 = LV_MIN(x1, sx + seg);
	}
	if (ba.x2 > ba.x1)
		lv_draw_rect(l, &r, &ba);
}

static void mini_cover(void)
{
	const char *cv = s.track >= 0 ? s.lib->albums[s.lib->tracks[s.track].aidx].cover : NULL;
	const lv_image_dsc_t *d = cover_get(cv, 36);
	if (lv_image_get_src(s.mini_img) != d)
		lv_image_set_src(s.mini_img, d);
	set_hidden(s.mini_img, !d);
}

static void update_np_labels(void)
{
	lv_label_set_text(s.mini_icon, s.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE);
	if (s.track < 0) {
		lv_label_set_text(s.mini_text, tx("Нет трека"));
		lv_label_set_text(s.mini_artist, "");
		int at, xh;   /* без второй строки — середина строчных на середине мини-плеера */
		font_xheight(s.f_item, &at, &xh);
		lv_obj_set_y(s.mini_text, lv_obj_get_height(s.mini) / 2 - (at - xh / 2));
		refresh_subs();
		return;
	}
	const lib_track_t *tr = &s.lib->tracks[s.track];
	const lib_album_t *al = &s.lib->albums[tr->aidx];
	lv_label_set_text(s.mini_text, tr->title);
	lv_label_set_text(s.mini_artist, tx(tr->artist));
	lv_obj_set_y(s.mini_text, 8);
	mini_cover();
	/* соседние альбомы для карусели плеера */
	int a = s.album_pos[tr->aidx], lo = al->podcast ? nmusic : 0, hi = al->podcast ? s.lib->nalbums : nmusic;
	const lib_album_t *pa = a > lo ? &s.lib->albums[s.album_order[a - 1]] : NULL;
	const lib_album_t *na = a + 1 < hi ? &s.lib->albums[s.album_order[a + 1]] : NULL;
	player_set_neighbors(pa ? pa->title : NULL, pa ? pa->cover : NULL, na ? na->title : NULL, na ? na->cover : NULL);
	s.np = (track_t){ tr->title, tx(tr->artist), al->title, s.np_format, (int)s.dur };
	player_set_track(&s.np);
	player_set_cover(al->cover);
	player_set_fav(s.pls && pl_find(&s.pls[PL_FAVORITES], s.track) >= 0);
	player_set_state(s.playing, s.pos, s.volume);
	lyrics_refresh();
	refresh_subs();
}

static void update_modes(void)
{
	static char left[24];
	if (s.sleep_end) {
		int sec = (int)((int32_t)(s.sleep_end - lv_tick_get()) / 1000);
		sec = sec < 0 ? 0 : sec;
		snprintf(left, sizeof(left), tx("Сон: %d:%02d"), sec / 60, sec % 60);
	}
	/* «Ещё» подсвечено, если там что-то включено: эквалайзер, таймер сна, повтор A-B */
	int on[PM_COUNT] = { s.shuffle, s.repeat != 0, player_lyrics_shown(), s.eq_on || s.sleep != SLEEP_OFF || s.ab };
	const char *lab[PM_COUNT] = { tx("Случайно"), tx(repeat_names[s.repeat]), tx("Текст"), s.sleep_end ? left : tx("Ещё") };
	player_set_modes(on, lab);
}

/* таймер сна отсчитывается с момента выбора */
static void sleep_start(void)
{
	s.sleep_end = s.sleep == SLEEP_OFF ? 0 : lv_tick_get() + (uint32_t)sleep_min[s.sleep] * 60000u;
	if (!s.sleep_end)
		audio_set_volume(s.volume);   /* если выключили посреди затухания */
	update_modes();
}

/* текст играющего трека: файл .lrc рядом с ним, перечитывается при смене трека; нет — ищется в LRCLIB (по Wi-Fi),
 * найденный сохраняется рядом с треком */
static lyrics_t lyr;
static int lyr_track = -1, lyr_wait = -1;   /* lyr_wait — для какого трека идёт поиск */

static void lyrics_file_of(int t, char *f, int cap)
{
	const lib_track_t *tr = &s.lib->tracks[t];
	lyrics_path(f, cap, tr->path, tr->start > 0 || tr->end > 0, tr->artist, tr->title);
}

static void lyrics_file(char *f, int cap)
{
	lyrics_file_of(s.track, f, cap);
}

/* тексты для всей библиотеки — в фоне, пока есть Wi-Fi: по одному треку, найденное — на карту рядом с треком.
 * Чего в LRCLIB нет — в .xmb/lyrics_none.txt (время и путь .lrc) и 30 дней не спрашивается. Запрос из плеера —
 * вне очереди: фоновый ждёт, пока он не кончится */
#define LYR_MISS_DAYS 30
static int lyr_bg = -1, lyr_scan;   /* трек, который ищется в фоне; с какого продолжать обход */
static uint32_t lyr_next_ms;        /* следующий фоновый запрос — не раньше */
static char **lyr_miss;
static int lyr_nmiss;

static int str_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

static void lyr_miss_file(char *out, int cap)
{
	snprintf(out, cap, "%s/.xmb/lyrics_none.txt", music_root());
}

static void lyr_miss_load(void)
{
	char f[300], line[1200];
	lyr_miss_file(f, sizeof(f));
	FILE *fp = fopen(f, "r");
	if (!fp)
		return;
	long long now = time(NULL);
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = 0;
		char *tab = strchr(line, '\t');
		if (!tab || now - atoll(line) > LYR_MISS_DAYS * 86400LL)
			continue;
		lyr_miss = realloc(lyr_miss, (lyr_nmiss + 1) * sizeof(char *));
		lyr_miss[lyr_nmiss++] = strdup(tab + 1);
	}
	fclose(fp);
	qsort(lyr_miss, lyr_nmiss, sizeof(char *), str_cmp);
}

static void lyr_miss_add(const char *lrc)
{
	char f[300];
	lyr_miss_file(f, sizeof(f));
	FILE *fp = fopen(f, "a");
	if (fp) {
		fprintf(fp, "%lld\t%s\n", (long long)time(NULL), lrc);
		fclose(fp);
	}
}

static void lyrics_prefetch(void)
{
	static int loaded;
	if (!s.lib || !s.settings_path[0] || lyr_bg >= 0 || lyr_wait >= 0 || lyr_scan >= s.lib->ntracks ||
	    !wifi_online() || (int32_t)(lv_tick_get() - lyr_next_ms) < 0)
		return;
	if (!loaded) {
		loaded = 1;
		lyr_miss_load();
	}
	for (int k = 0; k < 4 && lyr_scan < s.lib->ntracks; k++) {   /* пропуски — по нескольку за кадр */
		int t = lyr_scan++;
		const lib_track_t *tr = &s.lib->tracks[t];
		if (s.lib->albums[tr->aidx].podcast || !tr->artist[0] || !tr->title[0])
			continue;
		char f[1100];
		struct stat st;
		lyrics_file_of(t, f, sizeof(f));
		const char *key = f;
		if (!stat(f, &st) || (lyr_nmiss && bsearch(&key, lyr_miss, lyr_nmiss, sizeof(char *), str_cmp)))
			continue;
		lyrics_fetch(tr->artist, tr->title, s.lib->albums[tr->aidx].title, tr->dur, f);
		lyr_bg = t;
		return;
	}
}

static void lyrics_find(void)
{
	lyr_wait = s.track;
	if (lyr_bg >= 0)
		return;   /* фоновый запрос кончится — этот начнётся (lyrics_poll) */
	const lib_track_t *tr = &s.lib->tracks[s.track];
	char f[1100];
	lyrics_file(f, sizeof(f));
	lyrics_fetch(tr->artist, tr->title, s.lib->albums[tr->aidx].title, tr->dur, f);
}

static void lyrics_refresh(void)
{
	if (s.track == lyr_track)
		return;
	lyr_track = s.track;
	if (s.track >= 0) {
		char f[1100];
		lyrics_file(f, sizeof(f));
		lyrics_load(&lyr, f);
	} else {
		lyrics_free(&lyr);
	}
	player_set_lyrics(&lyr);
	if (s.track >= 0 && !lyr.n && player_lyrics_shown() && wifi_online())
		lyrics_find();   /* текст открыт — у следующего трека тоже искать */
}

static void lyrics_toggle(void)
{
	lyrics_refresh();
	if (player_lyrics_shown() || lyr.n) {
		player_show_lyrics(!player_lyrics_shown());
		return;
	}
	if (s.track < 0)
		return;
	if (!wifi_online()) {
		toast("%s", "Текста нет. С Wi-Fi найду его в LRCLIB");
		return;
	}
	toast("%s", "Ищу текст…");
	lyrics_find();
	player_show_lyrics(1);   /* найдётся — появится сам */
}

/* итог поиска в LRCLIB: найден — перечитать файл, если трек тот же */
static void lyrics_poll(void)
{
	int st = lyrics_fetch_poll();
	if (st == LF_IDLE) {
		lyrics_prefetch();
		return;
	}
	if (st == LF_BUSY)
		return;
	if (lyr_bg >= 0) {   /* итог фонового поиска */
		int t = lyr_bg;
		lyr_bg = -1;
		lyr_next_ms = lv_tick_get() + (st == LF_ERROR ? 60000 : 5000);   /* LRCLIB на частые запросы отвечает 503 */
		if (st == LF_NONE) {
			char f[1100];
			lyrics_file_of(t, f, sizeof(f));
			lyr_miss_add(f);
		}
		if (lyr_wait >= 0 && lyr_wait != t) {   /* пока шёл фоновый, текст попросили в плеере — теперь он */
			int w = lyr_wait;
			lyr_wait = -1;
			if (w == s.track)
				lyrics_find();
			return;
		}
		if (t != s.track || (st != LF_FOUND && lyr_wait < 0))
			return;   /* не играющий трек; у играющего, не найден, но в плеере не просили — молча */
		lyr_wait = t;   /* дальше — как итог запроса из плеера */
	}
	int same = lyr_wait == s.track;
	lyr_wait = -1;
	if (!same)
		return;
	if (st == LF_FOUND) {
		lyr_track = -1;
		lyrics_refresh();
	} else {
		toast("%s", st == LF_NONE ? "Текст не найден" : "LRCLIB не отвечает");
		if (!lyr.n)
			player_show_lyrics(0);
	}
	update_modes();
}

static void toggle_mode(int m)
{
	if (m == PM_SHUFFLE)
		s.shuffle = !s.shuffle;
	else if (m == PM_REPEAT)
		s.repeat = (s.repeat + 1) % 3;
	else if (m == PM_LYRICS)
		lyrics_toggle();
	else if (m == PM_MORE)
		player_more();
	plan_next();
	update_modes();
	refresh_subs();
}

/* ---------- плейлисты ---------- */

static void toast(const char *fmt, ...)
{
	char buf[160];
	va_list ap;
	va_start(ap, fmt);
	if (!strcmp(fmt, "%s"))
		snprintf(buf, sizeof(buf), "%s", tx(va_arg(ap, const char *)));
	else
		vsnprintf(buf, sizeof(buf), tx(fmt), ap);
	va_end(ap);
	lv_point_t sz;   /* длинное — в несколько строк: ширина по тексту, но не шире экрана */
	lv_text_get_size(&sz, buf, s.f_item, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_width(s.toast_lbl, sz.x < SCR_W - 64 ? sz.x + 1 : SCR_W - 64);
	lv_label_set_text(s.toast_lbl, buf);
	s.toast_t = 1.0;
	s.bhint_t = 0;   /* место сверху одно: уведомление вместо значка кнопки */
	set_hidden(s.bhint, true);
	lv_obj_move_foreground(s.toast);
	set_hidden(s.toast, false);
}

/* какой плейлист открыт списком на первом уровне; −1 — никакой (или «Новый плейлист») */
static int open_pl(void)
{
	return s.cat == CAT_PLAYLISTS && s.cat_col[CAT_PLAYLISTS].sel < s.npl ? s.cat_col[CAT_PLAYLISTS].sel : -1;
}

/* пункты категории «Плейлисты»: у треков — обложки их альбомов (список из разных альбомов).
 * Если открыт плейлист — его список обновляется на месте */
static void playlists_build(void)
{
	node_t *v = calloc(s.npl + 1, sizeof(node_t));
	for (int i = 0; i < s.npl; i++) {
		const playlist_t *p = &s.pls[i];
		v[i].icon = i == PL_FAVORITES ? LV_SYMBOL_OK : LV_SYMBOL_LIST;
		v[i].title = p->name;
		if (p->n) {
			node_t *k = track_nodes(p->tracks, p->n);
			for (int j = 0; j < p->n; j++)
				k[j].cover = s.lib->albums[s.lib->tracks[p->tracks[j]].aidx].cover;
			v[i].kids = k;   /* старые массивы не освобождаются: на них может ссылаться очередь */
			v[i].nkids = p->n;
		} else {
			v[i].sub = i == PL_FAVORITES ? "пусто — сердечко в плеере" : "пусто";
		}
	}
	v[s.npl] = (node_t){ .icon = LV_SYMBOL_PLUS, .title = "Новый плейлист", .action = A_NEWPL };
	cats[CAT_PLAYLISTS].items = v;
	cats[CAT_PLAYLISTS].count = s.npl + 1;
	column_t *c = &s.cat_col[CAT_PLAYLISTS];
	int sel = c->sel;
	column_fill(c, v, s.npl + 1);
	c->sel = sel <= s.npl ? sel : s.npl;
	c->pos = c->sel;
	int open = open_pl();
	if (open >= 0 && s.depth >= 1) {
		const node_t *n = &v[open];
		column_t *st = &s.stack[1];
		if (n->kids && s.depth == 1) {
			int ss = st->sel;
			column_fill(st, n->kids, n->nkids);
			st->sel = ss < n->nkids ? ss : n->nkids - 1;
			st->pos = st->sel;
		} else {
			s.depth = 0;   /* плейлист опустел или удалён */
		}
	}
}

static void fav_toggle_track(int track)
{
	playlist_t *f = &s.pls[PL_FAVORITES];
	int i = pl_find(f, track);
	if (i >= 0)
		pl_remove(f, i);
	else
		pl_add(f, track);
	pl_save(s.lib, f, s.pl_dir);
	playlists_build();
	if (track == s.track)
		player_set_fav(i < 0);
	toast(i < 0 ? "Добавлено в «%s»" : "Убрано из «%s»", tx(f->name));
}

static void toggle_fav(void)
{
	if (s.track >= 0 && s.pls)
		fav_toggle_track(s.track);
}

static void playlist_commit(void)
{
	int k = s.pl_choice;
	if (k == 0)
		k = pl_create(&s.pls, &s.npl, s.pl_dir);
	playlist_t *p = &s.pls[k];
	int added = 0;
	for (int i = 0; i < s.nadd; i++)
		if (pl_find(p, s.add_tracks[i]) < 0) {
			pl_add(p, s.add_tracks[i]);
			added++;
		}
	if (!added) {
		toast(s.nadd > 1 ? "Все уже в «%s»" : "Уже в «%s»", tx(p->name));
		return;
	}
	pl_save(s.lib, p, s.pl_dir);
	playlists_build();
	if (added > 1)
		toast("%d треков — в «%s»", added, tx(p->name));
	else
		toast("Добавлено в «%s»", tx(p->name));
}

/* выбор плейлиста для треков: новый или один из существующих (избранное — сердечком) */
static void playlist_menu_for(const int *tracks, int n)
{
	if (!s.pls || n <= 0)
		return;
	free(s.add_tracks);
	s.add_tracks = malloc(sizeof(int) * n);
	memcpy(s.add_tracks, tracks, sizeof(int) * n);
	s.nadd = n;
	int k = 0;
	s.pl_names[k++] = "Новый плейлист";
	for (int i = 1; i < s.npl && k < MAX_OPTS; i++)
		s.pl_names[k++] = s.pls[i].name;
	s.pl_names[k] = NULL;
	s.pl_choice = 0;
	s.pl_menu = (node_t){ .icon = LV_SYMBOL_PLUS, .title = "В плейлист", .opts = s.pl_names, .val = &s.pl_choice,
			      .commit = playlist_commit };
	options_open(&s.pl_menu);
}

/* «+» в плеере */
static void playlist_menu(void)
{
	if (s.track >= 0)
		playlist_menu_for(&s.track, 1);
}

/* ---------- экранная клавиатура: имя плейлиста ---------- */

/* ЙЦУКЕН и QWERTY; ⇧ и ⌫ — в третьем ряду, язык, цифры, закрыть и ✓ — в нижнем */
#define KB_ROW4(lang) lang, "1#", LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, ""
static const char *const kb_ru[] = {
	"й", "ц", "у", "к", "е", "н", "г", "ш", "щ", "з", "х", "\n",
	"ф", "ы", "в", "а", "п", "р", "о", "л", "д", "ж", "э", "\n",
	LV_SYMBOL_UP, "я", "ч", "с", "м", "и", "т", "ь", "б", "ю", LV_SYMBOL_BACKSPACE, "\n",
	KB_ROW4("ENG"),
};
static const char *const kb_RU[] = {
	"Й", "Ц", "У", "К", "Е", "Н", "Г", "Ш", "Щ", "З", "Х", "\n",
	"Ф", "Ы", "В", "А", "П", "Р", "О", "Л", "Д", "Ж", "Э", "\n",
	LV_SYMBOL_UP, "Я", "Ч", "С", "М", "И", "Т", "Ь", "Б", "Ю", LV_SYMBOL_BACKSPACE, "\n",
	KB_ROW4("ENG"),
};
static const char *const kb_en[] = {
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
	"a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
	LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
	KB_ROW4("РУС"),
};
static const char *const kb_EN[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	"A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
	LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	KB_ROW4("РУС"),
};
/* «Только английская»: без клавиши языка */
#define KB_ROW4_EN "1#", LV_SYMBOL_CLOSE, " ", LV_SYMBOL_OK, ""
static const char *const kb_en1[] = {
	"q", "w", "e", "r", "t", "y", "u", "i", "o", "p", "\n",
	"a", "s", "d", "f", "g", "h", "j", "k", "l", "\n",
	LV_SYMBOL_UP, "z", "x", "c", "v", "b", "n", "m", LV_SYMBOL_BACKSPACE, "\n",
	KB_ROW4_EN,
};
static const char *const kb_EN1[] = {
	"Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", "\n",
	"A", "S", "D", "F", "G", "H", "J", "K", "L", "\n",
	LV_SYMBOL_UP, "Z", "X", "C", "V", "B", "N", "M", LV_SYMBOL_BACKSPACE, "\n",
	KB_ROW4_EN,
};
#define KB_SPECIAL (LV_BUTTONMATRIX_CTRL_CHECKED | LV_BUTTONMATRIX_CTRL_NO_REPEAT | LV_BUTTONMATRIX_CTRL_CLICK_TRIG)
#define KB_CTRL4 KB_SPECIAL | 3, KB_SPECIAL | 3, KB_SPECIAL | 3, 8, KB_SPECIAL | 4
static const lv_buttonmatrix_ctrl_t kb_ctrl_ru[] = {
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	KB_SPECIAL | 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, KB_SPECIAL | 5,
	KB_CTRL4,
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_en[] = {
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4,
	KB_SPECIAL | 6, 4, 4, 4, 4, 4, 4, 4, KB_SPECIAL | 6,
	KB_CTRL4,
};
static const lv_buttonmatrix_ctrl_t kb_ctrl_en1[] = {
	4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
	4, 4, 4, 4, 4, 4, 4, 4, 4,
	KB_SPECIAL | 6, 4, 4, 4, 4, 4, 4, 4, KB_SPECIAL | 6,
	KB_SPECIAL | 3, KB_SPECIAL | 3, 11, KB_SPECIAL | 4,
};
static lv_keyboard_mode_t kb_lang = LV_KEYBOARD_MODE_USER_3;   /* последний язык: русский или латиница */

static void kb_close(void)
{
	if (s.kb_panel)
		lv_obj_delete(s.kb_panel);
	s.kb_panel = NULL;
	s.kb_info = NULL;
}

/* ---------- поиск: слова запроса в любом порядке, без регистра, ё = е ---------- */

#define SEARCH_MAX 400
static char search_q[64], search_sub[96];

static struct {
	unsigned c[8][32];
	int len[8], n;
} sq;

static int fold_str(const char *str, unsigned *out, int max)
{
	int n = 0;
	while (str && *str && n < max)
		out[n++] = lib_fold(&str);
	return n;
}

static void search_parse(const char *q)
{
	unsigned buf[64];
	int n = fold_str(q, buf, 64);
	sq.n = 0;
	for (int i = 0; i < n && sq.n < 8;) {
		while (i < n && buf[i] == ' ')
			i++;
		int st = i;
		while (i < n && buf[i] != ' ')
			i++;
		if (i > st) {
			sq.len[sq.n] = i - st > 32 ? 32 : i - st;
			memcpy(sq.c[sq.n], buf + st, sq.len[sq.n] * sizeof(unsigned));
			sq.n++;
		}
	}
}

/* каждое слово запроса есть в одной из строк */
static int search_match(const char *a, const char *b, const char *c)
{
	unsigned h[384];
	int hn = 0;
	const char *parts[3] = { a, b, c };
	for (int i = 0; i < 3; i++)
		if (parts[i] && hn < 383) {
			hn += fold_str(parts[i], h + hn, 383 - hn);
			h[hn++] = ' ';
		}
	for (int w = 0; w < sq.n; w++) {
		int found = 0;
		for (int i = 0; !found && i + sq.len[w] <= hn; i++)
			found = !memcmp(h + i, sq.c[w], sq.len[w] * sizeof(unsigned));
		if (!found)
			return 0;
	}
	return sq.n > 0;
}

/* найденное по порядку: исполнители, альбомы, треки. В out — первые max, в cnt — сколько всего каждого */
static int search_run(node_t *out, int max, int cnt[3])
{
	const library_t *L = s.lib;
	int n = 0;
	cnt[0] = cnt[1] = cnt[2] = 0;
	if (!L || !lib_artists || !sq.n)
		return 0;
	for (int i = 0; i < L->nartists; i++)
		if (search_match(L->artists[i].name, NULL, NULL)) {
			cnt[0]++;
			if (n < max)
				out[n++] = lib_artists[i];
		}
	for (int i = 0; i < nmusic; i++) {
		const lib_album_t *al = &L->albums[s.album_order[i]];
		if (search_match(al->title, al->artist, NULL)) {
			cnt[1]++;
			if (n < max)
				out[n++] = lib_albums[i];
		}
	}
	for (int i = 0; i < L->nby_title; i++) {
		int k = L->by_title[i];
		const lib_track_t *tr = &L->tracks[k];
		if (search_match(tr->title, tr->artist, L->albums[tr->aidx].title)) {
			cnt[2]++;
			if (n < max)
				out[n++] = (node_t){ .icon = LV_SYMBOL_AUDIO, .title = tr->title, .action = A_TRACK, .track = k,
						     .cover = L->albums[tr->aidx].cover };
		}
	}
	return n;
}

static void search_counts(char *buf, size_t sz, const int cnt[3])
{
	if (!cnt[0] && !cnt[1] && !cnt[2]) {
		snprintf(buf, sz, "%s", sq.n ? tx("ничего не найдено") : "");
		return;
	}
	snprintf(buf, sz, tx("исполнителей %d · альбомов %d · треков %d"), cnt[0], cnt[1], cnt[2]);
}

/* по мере набора: сколько найдено и первые четыре находки */
static void search_preview(void)
{
	node_t first[4];
	int cnt[3];
	char buf[96];
	search_parse(lv_textarea_get_text(s.kb_ta));
	int n = search_run(first, 4, cnt);
	search_counts(buf, sizeof(buf), cnt);
	lv_label_set_text(s.kb_info, buf);
	for (int i = 0; i < 4; i++) {
		lv_label_set_text_fmt(s.kb_prev[i], "%s  %s", i < n ? first[i].icon : "", i < n ? tx(first[i].title) : "");
		set_hidden(s.kb_prev[i], i >= n);
	}
}

static void search_ta_event(lv_event_t *e)
{
	(void)e;
	if (s.kb_info)
		search_preview();
}

/* ✓: список найденного открывается под «Поиском» */
static void search_done(void)
{
	snprintf(search_q, sizeof(search_q), "%s", lv_textarea_get_text(s.kb_ta));
	search_parse(search_q);
	int cnt[3];
	node_t *res = calloc(SEARCH_MAX, sizeof(node_t));   /* не освобождается: на список может ссылаться очередь */
	int n = search_run(res, SEARCH_MAX, cnt);
	if (!n) {
		free(res);
		toast("%s", sq.n ? "Ничего не найдено" : "Введите, что искать");
		return;
	}
	kb_close();
	snprintf(search_sub, sizeof(search_sub), "«%s»: %d", search_q, cnt[0] + cnt[1] + cnt[2]);
	music[MUSIC_SEARCH].sub = search_sub;
	refresh_subs();
	column_fill(&s.stack[1], res, n);
	s.stack[1].grid = 0;
	s.depth = 1;
	s.at_cats = 0;
	layout();
}

static void kb_done(void)
{
	if (s.kb_pl == KB_WIFI) {
		wifi_connect(wifi_kb_ssid, lv_textarea_get_text(s.kb_ta));
		toast("Подключение к «%s»…", wifi_kb_ssid);
		kb_close();
		return;
	}
	char name[64];
	snprintf(name, sizeof(name), "%s", lv_textarea_get_text(s.kb_ta));
	for (char *c = name; *c; c++)   /* символы, которых не бывает в именах файлов */
		if (strchr("/\\:*?\"<>|", *c))
			*c = '_';
	int n = (int)strlen(name);
	while (n && name[n - 1] == ' ')
		name[--n] = 0;
	if (!n) {
		toast("%s", "Пустое имя");
		return;
	}
	int fresh = s.kb_pl >= s.npl;
	if (fresh)
		s.kb_pl = pl_create(&s.pls, &s.npl, s.pl_dir);
	if (pl_rename(s.pls, s.npl, s.kb_pl, s.pl_dir, name)) {
		if (fresh)
			pl_delete(&s.pls, &s.npl, s.kb_pl);
		toast("Имя «%s» уже занято", name);
		return;
	}
	kb_close();
	if (fresh) {
		pl_save(s.lib, &s.pls[s.kb_pl], s.pl_dir);
		s.cat_col[CAT_PLAYLISTS].sel = s.kb_pl;
	}
	playlists_build();
	s.cat_col[CAT_PLAYLISTS].pos = s.cat_col[CAT_PLAYLISTS].sel;
	toast(fresh ? "Создан «%s»" : "Теперь «%s»", name);
}

static void kb_event(lv_event_t *e)
{
	lv_obj_t *kb = lv_event_get_target(e);
	const char *txt = lv_buttonmatrix_get_button_text(kb, lv_buttonmatrix_get_selected_button(kb));
	if (!txt)
		return;
	lv_keyboard_mode_t mode = lv_keyboard_get_mode(kb);
	if (!strcmp(txt, "ENG") || !strcmp(txt, "РУС")) {
		kb_lang = txt[0] == 'E' ? LV_KEYBOARD_MODE_USER_3 : LV_KEYBOARD_MODE_USER_1;
		lv_keyboard_set_mode(kb, kb_lang);
	} else if (!strcmp(txt, LV_SYMBOL_KEYBOARD) || !strcmp(txt, "abc"))   /* из цифр — на последний язык */
		lv_keyboard_set_mode(kb, kb_lang);
	else if (!strcmp(txt, LV_SYMBOL_UP))   /* заглавные: USER_1 ↔ USER_2, USER_3 ↔ USER_4 */
		lv_keyboard_set_mode(kb, mode == LV_KEYBOARD_MODE_USER_1 ? LV_KEYBOARD_MODE_USER_2
					 : mode == LV_KEYBOARD_MODE_USER_2 ? LV_KEYBOARD_MODE_USER_1
					 : mode == LV_KEYBOARD_MODE_USER_3 ? LV_KEYBOARD_MODE_USER_4
									   : LV_KEYBOARD_MODE_USER_3);
	else if (!strcmp(txt, LV_SYMBOL_OK) && !lv_textarea_get_text(s.kb_ta)[0])
		kb_close();   /* пусто — значит передумали: выйти, а не ругаться */
	else if (!strcmp(txt, LV_SYMBOL_OK) && s.kb_pl == -1)
		search_done();
	else if (!strcmp(txt, LV_SYMBOL_OK))
		kb_done();
	else if (!strcmp(txt, LV_SYMBOL_CLOSE))
		kb_close();
	else {
		lv_keyboard_def_event_cb(e);
		if (mode == LV_KEYBOARD_MODE_USER_2 || mode == LV_KEYBOARD_MODE_USER_4)   /* ⇧ — на одну букву */
			lv_keyboard_set_mode(kb, mode == LV_KEYBOARD_MODE_USER_2 ? LV_KEYBOARD_MODE_USER_1 : LV_KEYBOARD_MODE_USER_3);
	}
}

/* pl — переименовать плейлист (s.npl — создать новый), -1 — поиск, KB_WIFI — пароль сети */
static void kb_open(int pl, const char *title, const char *text)
{
	kb_close();
	s.kb_pl = pl;
	s.kb_panel = plain(lv_layer_top());
	lv_obj_set_clickable(s.kb_panel, true);   /* касания не проходят в меню под клавиатурой */
	lv_obj_set_size(s.kb_panel, SCR_W, SCR_H - TOP_H);
	lv_obj_set_pos(s.kb_panel, 0, TOP_H);   /* верхняя панель (часы, батарея) остаётся видна */
	lv_obj_set_style_bg_color(s.kb_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.kb_panel, 240, 0);
	lv_obj_t *t = label(s.kb_panel, s.f_title, tx(title));
	lv_obj_set_pos(t, 16, 6);
	s.kb_ta = lv_textarea_create(s.kb_panel);
	lv_textarea_set_one_line(s.kb_ta, true);
	lv_textarea_set_max_length(s.kb_ta, 40);
	lv_obj_set_style_text_font(s.kb_ta, s.f_item, 0);
	lv_obj_set_size(s.kb_ta, SCR_W - 32, 44);
	lv_obj_set_pos(s.kb_ta, 16, 38);
	lv_textarea_set_text(s.kb_ta, text);
	lv_textarea_set_max_length(s.kb_ta, pl == KB_WIFI ? 63 : 40);
	if (pl == -1) {
		s.kb_info = label(s.kb_panel, s.f_sub, "");
		lv_obj_set_pos(s.kb_info, 16, 88);
		lv_obj_set_style_text_opa(s.kb_info, 180, 0);
		for (int i = 0; i < 4; i++) {
			s.kb_prev[i] = label(s.kb_panel, s.f_sub, "");
			lv_obj_set_width(s.kb_prev[i], SCR_W - 32);
			lv_label_set_long_mode(s.kb_prev[i], LV_LABEL_LONG_MODE_DOTS);
			lv_obj_set_pos(s.kb_prev[i], 16, 110 + i * 22);
		}
		lv_obj_add_event_cb(s.kb_ta, search_ta_event, LV_EVENT_VALUE_CHANGED, NULL);
		search_preview();
	}
	lv_obj_t *kb = lv_keyboard_create(s.kb_panel);
	lv_obj_set_size(kb, SCR_W, 250);
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(kb, s.f_item, LV_PART_ITEMS);
	/* тёмная, как остальной интерфейс */
	lv_obj_set_style_bg_color(kb, lv_color_hex(0x101216), 0);
	lv_obj_set_style_border_width(kb, 0, 0);
	lv_obj_set_style_bg_color(kb, lv_color_hex(0x2c3038), LV_PART_ITEMS);
	lv_obj_set_style_bg_color(kb, lv_color_hex(0x41464f), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_bg_color(kb, lv_color_hex(ACCENT), LV_PART_ITEMS | LV_STATE_PRESSED);
	lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS);
	lv_obj_set_style_text_color(kb, lv_color_white(), LV_PART_ITEMS | LV_STATE_CHECKED);
	lv_obj_set_style_shadow_width(kb, 0, LV_PART_ITEMS);
	lv_obj_set_style_bg_color(s.kb_ta, lv_color_hex(0x2c3038), 0);
	lv_obj_set_style_text_color(s.kb_ta, lv_color_white(), 0);
	lv_obj_set_style_border_color(s.kb_ta, lv_color_hex(ACCENT), 0);
	/* курсор — светлый на тёмном поле; тема красит его только у поля в фокусе (а с клавиатурой оно в фокусе) */
	lv_obj_set_style_border_color(s.kb_ta, lv_color_white(), LV_PART_CURSOR | LV_STATE_FOCUSED);
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_1, kb_ru, kb_ctrl_ru);
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_2, kb_RU, kb_ctrl_ru);
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_3, s.kb_langs ? kb_en1 : kb_en, s.kb_langs ? kb_ctrl_en1 : kb_ctrl_en);
	lv_keyboard_set_map(kb, LV_KEYBOARD_MODE_USER_4, s.kb_langs ? kb_EN1 : kb_EN, s.kb_langs ? kb_ctrl_en1 : kb_ctrl_en);
	if (s.kb_langs || s.kb_start == 0)
		kb_lang = LV_KEYBOARD_MODE_USER_3;
	else if (s.kb_start == 1)
		kb_lang = LV_KEYBOARD_MODE_USER_1;
	lv_keyboard_set_mode(kb, kb_lang);
	lv_keyboard_set_textarea(kb, s.kb_ta);
	lv_obj_remove_event_cb(kb, lv_keyboard_def_event_cb);
	lv_obj_add_event_cb(kb, kb_event, LV_EVENT_VALUE_CHANGED, NULL);
}

/* ---------- действия с пунктом: долгий клик колесом или долгое касание ---------- */

enum { CA_FAV, CA_ADDPL, CA_REMOVE, CA_UP, CA_DOWN, CA_RENAME, CA_DELETE, CA_QNEXT, CA_QEND, CA_QREMOVE, CA_ERASE, CA_FORGET };

static struct {
	int pl, pos, track, choice, n, act[8];
	char erase[1024];         /* что удалить с карты: файл трека или папка */
	int erase_album, erase_dir;   /* альбом (−1 — нет) и «папка целиком» */
	const char *names[9];
	char label[96];
	int *tracks, ntracks;
	node_t node;
} cx;

static void ctx_commit(void);

/* ---------- удаление с карты ----------
 * Трек — его файл (трек из CUE-образа — нельзя: в файле весь альбом). Альбом — папка целиком, если в ней только он
 * (со сканами, обложками, cue), иначе — его файлы. Папка — целиком. Потом XMB перезапускается: библиотека, очередь,
 * плейлисты перечитываются, место в меню восстанавливается */
static int restart_req;
static void ctx_add(int act, const char *name);
static void toggle_play(void);

bool xmb_restart_requested(void)
{
	return restart_req;
}

static void erase_track(int track)
{
	const lib_track_t *tr = &s.lib->tracks[track];
	snprintf(cx.erase, sizeof(cx.erase), "%s", tr->path);
	cx.erase_album = -1;
	cx.erase_dir = tr->start > 0 || tr->end > 0 ? -1 : 0;
	ctx_add(CA_ERASE, "Удалить с карты…");
}

/* папка в «Папках»: путь — от корня карты по выбранным пунктам колонок */
static int folder_path(char *out, size_t cap)
{
	const lib_dir_t *d = &s.lib->root;
	int n = snprintf(out, cap, "%s", music_root());
	for (int k = 0; k <= s.depth; k++) {
		int i = col_at(k)->sel;
		if (i < 0 || i >= d->ndirs)
			return 0;
		d = &d->dirs[i];
		n += snprintf(out + n, cap - n, "/%s", d->name);
	}
	return n < (int)cap;
}

static void erase_group(const node_t *n)
{
	cx.erase_album = -1;
	cx.erase_dir = 1;
	if (s.cat == CAT_FOLDERS) {
		if (!folder_path(cx.erase, sizeof(cx.erase)))
			return;
	} else {   /* альбом: папка целиком, если все треки в ней (и в подпапках) — его */
		if (!n->kids || n->kids[0].action != A_TRACK)
			return;
		int a = s.lib->tracks[n->kids[0].track].aidx;
		const lib_album_t *al = &s.lib->albums[a];
		size_t dl = strlen(al->dir);
		cx.erase_album = a;
		snprintf(cx.erase, sizeof(cx.erase), "%s", al->dir);
		for (int i = 0; i < s.lib->ntracks; i++)
			if (s.lib->tracks[i].aidx != a && !strncmp(s.lib->tracks[i].path, al->dir, dl) && s.lib->tracks[i].path[dl] == '/')
				cx.erase_dir = 0;
	}
	ctx_add(CA_ERASE, "Удалить с карты…");
}

static int erase_one(const char *p, const struct stat *sb, int flag, struct FTW *f)
{
	(void)sb;
	(void)flag;
	(void)f;
	if (remove(p))
		perror(p);
	return 0;
}

static void nav_save(void);

static void erase_commit(void)
{
	const char *root = music_root();
	if (!root || strncmp(cx.erase, root, strlen(root)) || cx.erase[strlen(root)] != '/')
		return;   /* только внутри карты */
	if (s.playing)
		toggle_play();
	save_flush();
	audio_release();
	cover_hold(1);
	if (cx.erase_dir > 0) {
		nftw(cx.erase, erase_one, 16, FTW_DEPTH | FTW_PHYS);
	} else if (cx.erase_album >= 0) {   /* альбом делит папку с другими: только его файлы */
		const lib_album_t *al = &s.lib->albums[cx.erase_album];
		for (int i = 0; i < al->ntracks; i++)
			if (unlink(s.lib->tracks[al->tracks[i]].path) && errno != ENOENT)
				perror(s.lib->tracks[al->tracks[i]].path);
	} else if (unlink(cx.erase)) {
		perror(cx.erase);
	}
	sync();
	printf("erase: %s\n", cx.erase);
	nav_save();
	restart_req = 1;
}

static void ctx_add(int act, const char *name)
{
	cx.act[cx.n] = act;
	cx.names[cx.n++] = name;
	cx.names[cx.n] = NULL;
}

static void ctx_show(void)
{
	cx.choice = 0;
	cx.node = (node_t){ .icon = LV_SYMBOL_SETTINGS, .title = "Действия", .opts = cx.names, .val = &cx.choice,
			    .commit = ctx_commit };
	options_open(&cx.node);
}

static void ctx_open(void)
{
	if (s.at_cats || s.opt_open || player_shown() || !s.pls)
		return;
	column_t *c = cur_col();
	if (!c->count)
		return;
	const node_t *n = &c->nodes[c->sel];
	cx.n = 0;
	if (n->action == A_WIFINET) {
		if (!wifi_list[n->track].saved)
			return;
		cx.track = n->track;
		ctx_add(CA_FORGET, "Забыть сеть");
	} else if (s.cat == CAT_PLAYLISTS && s.depth == 0) {
		cx.pl = c->sel;
		if (cx.pl >= s.npl)
			return;
		if (cx.pl == PL_FAVORITES) {
			toast("%s", "«Избранное» не переименовывается и не удаляется");
			return;
		}
		ctx_add(CA_RENAME, "Переименовать");
		ctx_add(CA_DELETE, "Удалить плейлист");
	} else if (s.cat == CAT_MUSIC && s.depth == 1 && c->nodes == s.queue && n->action == A_TRACK) {   /* очередь */
		cx.pos = c->sel;
		cx.track = n->track;
		if (cx.pos != s.qi)
			ctx_add(CA_QREMOVE, "Убрать из очереди");
		ctx_add(CA_FAV, pl_find(&s.pls[PL_FAVORITES], n->track) >= 0 ? "Убрать из избранного" : "В избранное");
		ctx_add(CA_ADDPL, "В плейлист…");
	} else if (open_pl() >= 0 && s.depth == 1 && n->action == A_TRACK) {
		cx.pl = open_pl();
		cx.pos = c->sel;
		cx.track = n->track;
		ctx_add(CA_QNEXT, "Играть следующим");
		ctx_add(CA_QEND, "В конец очереди");
		ctx_add(CA_REMOVE, "Убрать из плейлиста");
		if (cx.pos > 0)
			ctx_add(CA_UP, "Выше");
		if (cx.pos + 1 < c->count)
			ctx_add(CA_DOWN, "Ниже");
	} else if (n->action == A_TRACK) {
		cx.track = n->track;
		ctx_add(CA_QNEXT, "Играть следующим");
		ctx_add(CA_QEND, "В конец очереди");
		ctx_add(CA_FAV, pl_find(&s.pls[PL_FAVORITES], n->track) >= 0 ? "Убрать из избранного" : "В избранное");
		ctx_add(CA_ADDPL, "В плейлист…");
		erase_track(n->track);
	} else if (n->kids && n->nkids && n->kids[0].action == A_TRACK) {   /* альбом или папка с треками */
		free(cx.tracks);
		cx.tracks = malloc(sizeof(int) * n->nkids);
		cx.ntracks = 0;
		for (int i = 0; i < n->nkids; i++)
			if (n->kids[i].action == A_TRACK)
				cx.tracks[cx.ntracks++] = n->kids[i].track;
		cx.track = -1;
		ctx_add(CA_QNEXT, "Играть следующим");
		ctx_add(CA_QEND, "В конец очереди");
		ctx_add(CA_ADDPL, "Все треки — в плейлист…");
		erase_group(n);
	} else if (s.cat == CAT_FOLDERS && n->kids) {   /* папка только с подпапками */
		cx.ntracks = 0;
		erase_group(n);
	} else {
		return;
	}
	ctx_show();
}

static void pl_delete_do(void)
{
	if (cx.pl < 0 || cx.pl >= s.npl)
		return;
	char name[64];
	snprintf(name, sizeof(name), "%s", s.pls[cx.pl].name);
	pl_delete(&s.pls, &s.npl, cx.pl);
	playlists_build();
	toast("Удалён «%s»", name);
}

static void ctx_commit(void)
{
	playlist_t *p = cx.pl >= 0 && cx.pl < s.npl ? &s.pls[cx.pl] : NULL;
	switch (cx.act[cx.choice]) {
	case CA_FORGET:
		wifi_forget(wifi_list[cx.track].ssid);
		break;
	case CA_FAV:
		fav_toggle_track(cx.track);
		break;
	case CA_QNEXT:
	case CA_QEND:
		if (cx.track >= 0)
			q_add(&cx.track, 1, cx.act[cx.choice] == CA_QNEXT);
		else
			q_add(cx.tracks, cx.ntracks, cx.act[cx.choice] == CA_QNEXT);
		break;
	case CA_QREMOVE:
		q_remove(cx.pos);
		break;
	case CA_ERASE:   /* подтверждение: по умолчанию — «Отмена» */
		if (cx.erase_dir < 0) {
			toast("%s", "Трек внутри образа — удалите альбом целиком");
			break;
		}
		snprintf(cx.label, sizeof(cx.label), tx("«%s» удалится с карты насовсем"),
			 cx.erase + (strrchr(cx.erase, '/') ? strrchr(cx.erase, '/') - cx.erase + 1 : 0));
		slide_open(LV_SYMBOL_TRASH, "Удалить с карты?", cx.label, "удалить", 1, "Отмена", erase_commit, NULL);
		break;
	case CA_ADDPL:
		if (cx.track >= 0)
			playlist_menu_for(&cx.track, 1);
		else
			playlist_menu_for(cx.tracks, cx.ntracks);
		break;
	case CA_REMOVE:
		pl_remove(p, cx.pos);
		pl_save(s.lib, p, s.pl_dir);
		playlists_build();
		toast("Убрано из «%s»", tx(p->name));
		break;
	case CA_UP:
	case CA_DOWN: {
		int d = cx.act[cx.choice] == CA_UP ? -1 : 1;
		pl_move(p, cx.pos, d);
		pl_save(s.lib, p, s.pl_dir);
		playlists_build();
		s.stack[1].sel = cx.pos + d;
		break;
	}
	case CA_RENAME:
		kb_open(cx.pl, "Название плейлиста", s.pls[cx.pl].name);
		break;
	case CA_DELETE:   /* подтверждение */
		snprintf(cx.label, sizeof(cx.label), tx("«%s» — треки останутся на карте"), tx(p->name));
		slide_open(LV_SYMBOL_TRASH, "Удалить плейлист?", cx.label, "удалить", 1, "Отмена", pl_delete_do, NULL);
		break;
	default:
		break;
	}
}

/* ---------- играющий трек в библиотеке ---------- */

static void set_cat(int c);

/* плеер закрывается, XMB раскрывается по пути (номера пунктов от уровня категории), курсор — на последнем */
static void reveal(int cat, const int *path, int n)
{
	if (player_shown())
		player_show(0);
	if (cat != s.cat) {
		set_cat(cat);
	} else {
		for (int d = 1; d <= s.depth; d++)
			hide_column(&s.stack[d]);
		s.depth = 0;
	}
	s.at_cats = 0;
	column_t *c = col_at(0);
	for (int k = 0; k < n && path[k] >= 0 && c->count > 0; k++) {
		c->sel = path[k] < c->count ? path[k] : c->count - 1;
		c->pos = c->sel;
		c->rpos = c->grid ? c->sel / GRID_COLS : 0;
		const node_t *nd = &c->nodes[c->sel];
		if (k + 1 == n || !nd->kids || s.depth + 1 >= MAX_DEPTH)
			break;
		column_fill(&s.stack[s.depth + 1], nd->kids, nd->nkids);
		s.stack[s.depth + 1].grid = nd->grid && s.album_view == 0;
		s.stack[s.depth + 1].big = nd->grid && s.album_view == 2;
		s.depth++;
		c = &s.stack[s.depth];
	}
	refresh_subs();
	layout();
}

static void reveal_album(void)
{
	if (s.track < 0 || cats[CAT_MUSIC].items != music)
		return;
	int a = s.lib->tracks[s.track].aidx, path[3] = { MUSIC_ALBUMS, s.album_pos[a], 0 };
	const lib_album_t *al = &s.lib->albums[a];
	if (al->podcast) {   /* «Подкасты»: [0] — «Обновить» */
		path[0] = MUSIC_PODCASTS;
		path[1] = 1 + s.album_pos[a] - nmusic;
	}
	for (int j = 0; j < al->ntracks; j++)
		if (al->tracks[j] == s.track)
			path[2] = j;
	reveal(CAT_MUSIC, path, 3);
}

/* путь к треку в дереве папок: в каждой папке сначала подпапки, потом треки — как в списке */
static int dir_path(const lib_dir_t *d, int track, int *path, int depth)
{
	if (depth >= MAX_DEPTH)
		return -1;
	for (int j = 0; j < d->ntracks; j++)
		if (d->tracks[j] == track) {
			path[depth] = d->ndirs + j;
			return depth + 1;
		}
	for (int i = 0; i < d->ndirs; i++) {
		path[depth] = i;
		int n = dir_path(&d->dirs[i], track, path, depth + 1);
		if (n > 0)
			return n;
	}
	return -1;
}

static void reveal_dir(void)
{
	int path[MAX_DEPTH];
	int n = s.track >= 0 && s.lib ? dir_path(&s.lib->root, s.track, path, 0) : -1;
	if (n > 0)
		reveal(CAT_FOLDERS, path, n);
}

/* ---------- меню «Ещё» в плеере: редкие функции ---------- */

enum { PX_QUEUE, PX_ALBUM, PX_DIR, PX_EQ, PX_PRESET, PX_SLEEP, PX_AB, PX_GAIN, PX_RG };

static const node_t px_preset = OPT(LV_SYMBOL_LIST, "Пресет эквалайзера", eq_presets, &s.eq_preset, eq_apply);
static const node_t px_sleep = OPT(LV_SYMBOL_POWER, "Таймер сна", sleep_names, &s.sleep, sleep_start);
static const node_t px_gain = OPT(LV_SYMBOL_VOLUME_MAX, "Усиление", gain_names, &s.gain, apply_gain);
static const node_t px_rg = OPT(LV_SYMBOL_SHUFFLE, "ReplayGain", rg_names, &s.rg_mode, apply_rg);

static struct {
	int choice, n, act[10];
	const char *names[11];
	char buf[10][64];
	node_t node;
} px;

static void px_add(int act, const char *fmt, const char *arg)
{
	snprintf(px.buf[px.n], sizeof(px.buf[0]), tx(fmt), tx(arg));
	px.act[px.n] = act;
	px.names[px.n] = px.buf[px.n];
	px.names[++px.n] = NULL;
}

static void ab_reset(void)
{
	s.ab = 0;
	player_set_ab(-1, -1);
}

static void px_commit(void)
{
	switch (px.act[px.choice]) {
	case PX_QUEUE: {
		int path[2] = { MUSIC_QUEUE, s.qi };
		reveal(CAT_MUSIC, path, 2);
		return;
	}
	case PX_ALBUM:
		reveal_album();
		return;
	case PX_DIR:
		reveal_dir();
		return;
	case PX_EQ:
		s.eq_on = !s.eq_on;
		eq_apply();
		toast(s.eq_on ? "Эквалайзер: %s" : "Эквалайзер выключен%s", s.eq_on ? tx(eq_presets[s.eq_preset]) : "");
		break;
	case PX_PRESET:
		if (!s.eq_on) {
			s.eq_on = 1;
			eq_apply();
		}
		options_open(&px_preset);
		break;
	case PX_SLEEP:
		options_open(&px_sleep);
		break;
	case PX_GAIN:
		options_open(&px_gain);
		break;
	case PX_RG:
		options_open(&px_rg);
		break;
	case PX_AB:
		if (s.track < 0 || s.resume)
			break;
		if (s.ab == 0) {
			s.ab = 1;
			s.ab_a = s.pos;
			player_set_ab(s.ab_a / s.dur, -1);
			toast("%s", "A отмечена — теперь отметьте B");
		} else if (s.ab == 1 && s.pos > s.ab_a + 1) {
			s.ab = 2;
			s.ab_b = s.pos;
			player_set_ab(s.ab_a / s.dur, s.ab_b / s.dur);
			seek_to(s.ab_a);
			s.pos = s.ab_a;
			toast("%s", "Повтор A-B");
		} else {
			ab_reset();
			toast("%s", "Повтор A-B выключен");
		}
		break;
	}
	update_modes();
}

static void player_more(void)
{
	px.n = 0;
	if (s.track >= 0) {
		px_add(PX_QUEUE, "%s", "Показать очередь");
		px_add(PX_ALBUM, "%s", "Перейти к альбому");
		px_add(PX_DIR, "%s", "Перейти к папке");
	}
	px_add(PX_EQ, "%s", s.eq_on ? "Выключить эквалайзер" : "Включить эквалайзер");
	px_add(PX_PRESET, "Пресет: %s", eq_presets[s.eq_preset]);
	px_add(PX_SLEEP, "Таймер сна: %s", sleep_names[s.sleep]);
	px_add(PX_AB, "%s", s.ab == 0 ? "Повтор A-B: отметить A" : s.ab == 1 ? "Повтор A-B: отметить B" : "Выключить повтор A-B");
	px_add(PX_GAIN, "Усиление: %s", gain_names[s.gain]);
	px_add(PX_RG, "ReplayGain: %s", rg_names[s.rg_mode]);
	px.choice = 0;
	px.node = (node_t){ .icon = LV_SYMBOL_SETTINGS, .title = "Ещё", .opts = px.names, .val = &px.choice, .commit = px_commit };
	options_open(&px.node);
}

/* ---------- последний трек: файл рядом с настройками ---------- */

static void last_file(char *out, size_t cap)
{
	snprintf(out, cap, "%s", s.settings_path);
	char *sl = strrchr(out, '/');
	if (sl)
		snprintf(sl + 1, cap - (size_t)(sl + 1 - out), "last.txt");
}

static void last_save(void)
{
	s.last_saved_ms = lv_tick_get();
	if (s.track < 0 || !s.settings_path[0])
		return;
	char f[320];
	last_file(f, sizeof(f));
	const lib_track_t *tr = &s.lib->tracks[s.track];
	char buf[1200];
	int n = snprintf(buf, sizeof(buf), "path=%s\nstart=%.3f\npos=%.1f\nqi=%d\n", tr->path, tr->start,
			 s.resume ? s.resume_pos : s.pos, s.qi);
	fsave(f, buf, n < (int)sizeof(buf) ? (size_t)n : sizeof(buf) - 1);
}

/* при старте: последний трек прошлого запуска — в мини-плеер, на паузе, очередь — его альбом */
/* ---------- очередь: свой список треков ----------
 * Запуск из списка (альбом, папка, плейлист, поиск) копирует его треки в очередь; «играть следующим» вставляет
 * сразу за текущим (и за уже поставленными так же), «в конец» — добавляет. Список очереди — пункт «Очередь»
 * в «Музыке»; сохраняется рядом с настройками (queue.txt) и восстанавливается при запуске */
static node_t *q_nodes;
static int q_len, q_cap;

static node_t q_node(int track)
{
	const lib_track_t *tr = &s.lib->tracks[track];
	return (node_t){ .icon = LV_SYMBOL_AUDIO, .title = tr->title, .action = A_TRACK, .track = track,
			 .cover = s.lib->albums[tr->aidx].cover };
}

static void q_reserve(int n)
{
	if (n <= q_cap)
		return;
	q_cap = n + 64;
	q_nodes = realloc(q_nodes, sizeof(node_t) * q_cap);
}

static void q_file(char *out, size_t cap)
{
	last_file(out, cap);
	char *sl = strrchr(out, '/');
	if (sl)
		snprintf(sl + 1, cap - (size_t)(sl + 1 - out), "queue.txt");
}

/* очередь и последний трек пишутся через 2 с после последнего изменения (frame_cb) фоновой записью (filesave),
 * перед сном, USB, стоком, удалением — сразу и с ожиданием */
static uint32_t q_dirty, last_dirty;

static void q_save(void)
{
	q_dirty = lv_tick_get() | 1;
}

static void last_save_later(void)
{
	last_dirty = lv_tick_get() | 1;
	s.last_saved_ms = lv_tick_get();
}

static void q_write(void);

static void save_flush(void)
{
	if (q_dirty)
		q_write();
	q_dirty = last_dirty = 0;
	last_save();
	fsave_flush();
}

static void q_write(void)
{
	char f[320], *buf = NULL;
	size_t len = 0;
	if (!s.settings_path[0])
		return;
	q_file(f, sizeof(f));
	FILE *fp = open_memstream(&buf, &len);
	if (!fp)
		return;
	for (int i = 0; i < q_len; i++)
		fprintf(fp, "%s\t%.3f\n", s.lib->tracks[q_nodes[i].track].path, s.lib->tracks[q_nodes[i].track].start);
	fclose(fp);
	fsave(f, buf, len);
	free(buf);
}

/* очередь изменилась: открытый её список, пункт «Очередь», заказанный движку следующий, файл */
static void q_changed(const node_t *old)
{
	s.queue = q_nodes;
	s.qlen = q_len;
	for (int d = 1; d <= s.depth; d++) {
		column_t *c = &s.stack[d];
		if (c->nodes != old && c->nodes != q_nodes)
			continue;
		int sel = c->sel;
		column_fill(c, q_nodes, q_len);
		c->sel = sel < q_len ? sel : q_len - 1;
		c->sel = c->sel < 0 ? 0 : c->sel;
		c->pos = c->sel;
	}
	if (cats[CAT_MUSIC].items == music) {
		music[MUSIC_QUEUE].kids = q_len ? q_nodes : NULL;
		music[MUSIC_QUEUE].nkids = q_len;
		music[MUSIC_QUEUE].sub = q_len ? NULL : "пусто";
		refresh_subs();
	}
	plan_next();
	q_save();
}

/* очередь — треки списка q (без папок и прочего); возвращает место трека qi в ней */
/* новая очередь — из списка, откуда запущен трек. Поставленное вручную «играть следующим» и ещё не сыгранное
 * остаётся сразу за ним: другой альбом (в том числе случайный свайп в плеере) меняет только то, что дальше */
static int q_set(const node_t *q, int qlen, int qi)
{
	const node_t *old = q_nodes;
	int keep = s.up_next > 0 && s.qi + s.up_next < q_len ? s.up_next : 0;
	node_t *pend = keep ? malloc(sizeof(node_t) * keep) : NULL;
	if (pend)
		memcpy(pend, q_nodes + s.qi + 1, sizeof(node_t) * keep);
	else
		keep = 0;
	int n = 0, at = 0;
	q_reserve(qlen + keep);
	for (int i = 0; i < qlen; i++)
		if (q[i].action == A_TRACK) {
			if (i == qi)
				at = n;
			q_nodes[n++] = q_node(q[i].track);
			if (i == qi && keep) {
				memcpy(q_nodes + n, pend, sizeof(node_t) * keep);
				n += keep;
			}
		}
	free(pend);
	q_len = n;
	s.up_next = keep;
	s.qi = at;
	q_changed(old);
	return at;
}

static void play_queue(const node_t *q, int qlen, int qi);

static void q_add(const int *tracks, int n, int next)
{
	if (n <= 0)
		return;
	if (s.track < 0 || !q_len) {   /* ничего не играло: очередь — эти треки, сразу играть */
		node_t *v = malloc(sizeof(node_t) * n);
		for (int i = 0; i < n; i++)
			v[i] = q_node(tracks[i]);
		play_queue(v, n, 0);
		free(v);
		return;
	}
	const node_t *old = q_nodes;
	int at = next ? s.qi + 1 + s.up_next : q_len;
	at = at > q_len ? q_len : at;
	q_reserve(q_len + n);
	memmove(q_nodes + at + n, q_nodes + at, sizeof(node_t) * (q_len - at));
	for (int i = 0; i < n; i++)
		q_nodes[at + i] = q_node(tracks[i]);
	q_len += n;
	if (next)
		s.up_next += n;
	q_changed(old);
	if (n == 1)
		toast(next ? "Следующим: %s" : "В конце очереди: %s", s.lib->tracks[tracks[0]].title);
	else
		toast(next ? "Следующими — %d треков" : "В конец очереди — %d треков", n);
}

static void q_remove(int pos)
{
	if (pos < 0 || pos >= q_len || pos == s.qi)
		return;
	memmove(q_nodes + pos, q_nodes + pos + 1, sizeof(node_t) * (q_len - pos - 1));
	q_len--;
	if (pos < s.qi)
		s.qi--;
	else if (pos <= s.qi + s.up_next && s.up_next > 0)
		s.up_next--;
	q_changed(q_nodes);
}

static int path_cmp(const void *a, const void *b)
{
	const lib_track_t *x = &s.lib->tracks[*(const int *)a], *y = &s.lib->tracks[*(const int *)b];
	int c = strcmp(x->path, y->path);
	return c ? c : x->start < y->start ? -1 : x->start > y->start;
}

/* очередь из queue.txt: пути, которых больше нет в библиотеке, пропускаются */
static int q_load(void)
{
	char f[320], line[1100];
	q_file(f, sizeof(f));
	FILE *fp = fopen(f, "r");
	if (!fp)
		return 0;
	int nt = s.lib->ntracks, *idx = malloc(sizeof(int) * (nt ? nt : 1));
	for (int i = 0; i < nt; i++)
		idx[i] = i;
	qsort(idx, nt, sizeof(int), path_cmp);
	q_len = 0;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = 0;
		char *tab = strrchr(line, '\t');
		if (!tab)
			continue;
		*tab = 0;
		double start = atof(tab + 1);
		int lo = 0, hi = nt - 1, found = -1;
		while (lo <= hi) {
			int mid = (lo + hi) / 2;
			const lib_track_t *tr = &s.lib->tracks[idx[mid]];
			int c = strcmp(line, tr->path);
			if (!c)
				c = start < tr->start - 0.01 ? -1 : start > tr->start + 0.01;
			if (!c) {
				found = idx[mid];
				break;
			}
			if (c < 0)
				hi = mid - 1;
			else
				lo = mid + 1;
		}
		if (found >= 0) {
			q_reserve(q_len + 1);
			q_nodes[q_len++] = q_node(found);
		}
	}
	fclose(fp);
	free(idx);
	return q_len;
}

/* трек s.track выбран, но не открыт: играть начнёт toggle_play с позиции pos; формат — по заголовку файла (не
 * вышло — по расширению) */
static void np_select(double pos)
{
	const lib_track_t *tr = &s.lib->tracks[s.track];
	s.dur = tr->dur;
	s.pos = s.resume_pos = pos;
	s.resume = 1;
	s.playing = 0;
	char codec[8];
	int rate, bits;
	if (!audio_probe(tr->path, codec, &rate, &bits) && rate && bits) {
		snprintf(s.np_format, sizeof(s.np_format), "%s · %d bit / %g kHz", codec, bits, rate / 1000.0);
	} else {
		const char *e = strrchr(tr->path, '.');
		snprintf(s.np_format, sizeof(s.np_format), "%s", e ? e + 1 : "");
		for (char *c = s.np_format; *c; c++)
			*c = (char)(*c >= 'a' && *c <= 'z' ? *c - 32 : *c);
	}
	update_np_labels();
	player_set_state(0, s.pos, s.volume);
}

static void last_load(void)
{
	char f[320], line[1100], path[1024] = "";
	double start = 0, pos = 0;
	int qi = -1;
	if (!s.settings_path[0])
		return;
	last_file(f, sizeof(f));
	FILE *fp = fopen(f, "r");
	if (!fp)
		return;
	while (fgets(line, sizeof(line), fp)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "path=", 5))
			snprintf(path, sizeof(path), "%s", line + 5);
		else if (!strncmp(line, "start=", 6))
			start = atof(line + 6);
		else if (!strncmp(line, "pos=", 4))
			pos = atof(line + 4);
		else if (!strncmp(line, "qi=", 3))
			qi = atoi(line + 3);
	}
	fclose(fp);
	for (int i = 0; i < s.lib->ntracks; i++) {
		const lib_track_t *tr = &s.lib->tracks[i];
		if (strcmp(tr->path, path) || fabs(tr->start - start) > 0.01)
			continue;
		const lib_album_t *al = &s.lib->albums[tr->aidx];
		s.track = i;
		if (q_load() && qi >= 0 && qi < q_len && q_nodes[qi].track == i) {
			s.qi = qi;
			q_changed(q_nodes);
		} else {   /* очереди нет или она не про этот трек — альбом трека */
			int k = 0;
			while (k < al->ntracks && al->tracks[k] != i)
				k++;
			q_set(s.album_nodes[tr->aidx], al->ntracks, k);
		}
		np_select(pos < tr->dur ? pos : 0);
		return;
	}
}

/* ReplayGain трека: по альбому — если есть альбомный тег, иначе по треку; нет тега — без изменений */
static void track_rg(int track, float *db, float *peak)
{
	*db = *peak = 0;
	if (!s.rg_mode || track < 0 || !s.lib)
		return;
	const lib_track_t *tr = &s.lib->tracks[track];
	int album = s.rg_mode == 2 && tr->rg_album != LIB_RG_NONE;
	float g = album ? tr->rg_album : tr->rg_track, pk = album ? tr->rg_apeak : tr->rg_tpeak;
	if (g != LIB_RG_NONE) {
		*db = g;
		*peak = pk != LIB_RG_NONE ? pk : 0;
	}
}

/* какой трек очереди следующий: повтор одного — он же, случайно — любой, в конце очереди — по кругу или нет */
static int queue_next(void)
{
	if (!s.queue || s.qlen <= 0)
		return -1;
	if (s.up_next > 0 && s.qi + 1 < s.qlen)
		return s.qi + 1;   /* поставленное «играть следующим» — раньше повтора и перемешивания */
	if (s.repeat == 2)
		return s.qi;
	int n = s.shuffle ? rand() % s.qlen : s.qi + 1;
	if (n >= s.qlen)
		n = s.repeat == 1 ? 0 : -1;
	return n >= 0 && s.queue[n].action == A_TRACK ? n : -1;
}

/* заказать движку следующий трек: заиграет без паузы. Кроссфейд — кроме соседних треков одного альбома */
static void plan_next(void)
{
	s.next_qi = s.track >= 0 && !s.resume ? queue_next() : -1;
	if (s.next_qi < 0) {
		audio_set_next(NULL, 0, 0, 0, 0, 0, 0);
		return;
	}
	int nt = s.queue[s.next_qi].track;
	const lib_track_t *cur = &s.lib->tracks[s.track], *tr = &s.lib->tracks[nt];
	int album = tr->aidx == cur->aidx && tr->disc == cur->disc && tr->track == cur->track + 1;
	float db, peak;
	track_rg(nt, &db, &peak);
	audio_set_next(tr->path, tr->start, tr->end, tr->dur, db, peak, !album);
}

static void apply_xfade(void)
{
	audio_set_crossfade(s.xfade * 2000);
}

/* интерфейс — на трек qi очереди (звук уже идёт или запущен) */
static void np_switch(const node_t *q, int qlen, int qi)
{
	if (s.up_next > 0 && qi == s.qi + 1)
		s.up_next--;   /* «играть следующим» отыграл */
	else if (qi != s.qi)
		s.up_next = 0;
	s.queue = q;
	s.qlen = qlen;
	s.qi = qi;
	s.track = q[qi].track;
	const lib_track_t *tr = &s.lib->tracks[s.track];
	audio_status_t st;
	audio_get_status(&st);
	if (st.bits && st.rate)
		snprintf(s.np_format, sizeof(s.np_format), "%s · %d bit / %g kHz", st.codec, st.bits, st.rate / 1000.0);
	else
		snprintf(s.np_format, sizeof(s.np_format), "%s", st.codec);
	s.pos = 0;
	s.dur = st.dur > 0 ? st.dur : tr->dur;
	s.playing = 1;
	s.ended_seen = 0;
	s.resume = 0;
	ab_reset();
	if (s.lib->albums[tr->aidx].podcast && pod_pos(tr->path) > 5)
		seek_to(pod_pos(tr->path));   /* выпуск — с того места, где остановился */
	update_np_labels();
	last_save_later();
	plan_next();
}

/* трек qi из очереди (списка, откуда он запущен) */
static void play_queue(const node_t *q, int qlen, int qi)
{
	if (!q || qi < 0 || qi >= qlen || q[qi].action != A_TRACK)
		return;
	int track = q[qi].track;
	if (q != q_nodes) {   /* запуск из другого списка: он и становится очередью */
		qi = q_set(q, qlen, qi);
		q = q_nodes;
		qlen = q_len;
	}
	const lib_track_t *tr = &s.lib->tracks[track];
	float db, peak;
	track_rg(track, &db, &peak);
	audio_set_replaygain(db, peak);
	audio_play_range(tr->path, tr->start, tr->end, tr->dur);
	np_switch(q, qlen, qi);
}

static void seek_to(double sec)
{
	audio_seek(sec);
	s.pos = s.seek_hold = sec;
	s.seek_ms = lv_tick_get();
}

/* пауза / продолжение; последний трек прошлого запуска открывается с того места, где остановились */
static void toggle_play(void)
{
	if (s.track < 0)
		return;
	if (s.resume) {
		double p = s.resume_pos;
		play_queue(s.queue, s.qlen, s.qi);
		if (p > 1) {
			seek_to(p);
			s.pos = p;
		}
		return;
	}
	audio_pause(s.playing);
	s.playing = !s.playing;
	update_np_labels();
	last_save_later();
}

/* by_end — трек доиграл сам: «повтор одного» играет его снова. Кнопкой (свайпом, из меню) — всегда следующий,
 * режим повтора остаётся */
static void next_track(int dir, int by_end)
{
	if (!s.queue)
		return;
	if (dir > 0 && s.next_qi >= 0 && s.up_next > 0) {
		play_queue(s.queue, s.qlen, s.next_qi);
		return;
	}
	if (s.repeat == 2 && by_end) {
		play_queue(s.queue, s.qlen, s.qi);
		return;
	}
	/* заказанный следующий при «повторе одного» — сам текущий трек: для кнопки он не подходит */
	int planned = s.next_qi >= 0 && s.next_qi != s.qi;
	int n = s.shuffle && dir > 0 && planned ? s.next_qi : s.shuffle ? rand() % s.qlen : s.qi + dir;
	if (n < 0 || n >= s.qlen) {
		if (s.repeat != 1) {   /* конец очереди без повтора — стоп на первом треке, «играть» — с начала */
			audio_pause(1);
			s.up_next = 0;
			s.qi = 0;
			s.track = s.queue[0].track;
			ab_reset();
			np_select(0);
			plan_next();
			last_save_later();
			toast("%s", "Конец очереди");
			return;
		}
		n = (n + s.qlen) % s.qlen;
	}
	play_queue(s.queue, s.qlen, n);
}

/* как на Walkman: трек играет дольше 3 с — в его начало, иначе предыдущий в очереди */
static void prev_track(void)
{
	if (!s.queue)
		return;
	if (s.resume && s.resume_pos > 3) {
		s.resume_pos = s.pos = 0;
		player_set_state(s.playing, s.pos, s.volume);
		return;
	}
	if (s.pos > 3 && !s.resume) {
		seek_to(0);
		s.pos = 0;
		player_set_state(s.playing, s.pos, s.volume);
		return;
	}
	int n = s.qi - 1;
	if (n < 0)
		n = s.repeat == 1 ? s.qlen - 1 : 0;
	play_queue(s.queue, s.qlen, n);
}

static int switch_album(int dir);

/* следующий альбом в очереди (папка с несколькими альбомами, «Все треки»), иначе — соседний по карусели */
static void next_group(void)
{
	if (!s.queue || s.track < 0)
		return;
	int a = s.lib->tracks[s.track].aidx;
	for (int i = s.qi + 1; i < s.qlen; i++)
		if (s.queue[i].action == A_TRACK && s.lib->tracks[s.queue[i].track].aidx != a) {
			play_queue(s.queue, s.qlen, i);
			player_release(1);
			return;
		}
	if (switch_album(1))
		player_release(1);
}

/* предыдущий альбом в очереди, иначе — соседний по карусели */
static void prev_group(void)
{
	if (!s.queue || s.track < 0)
		return;
	int a = s.lib->tracks[s.track].aidx, i = s.qi - 1;
	while (i >= 0 && (s.queue[i].action != A_TRACK || s.lib->tracks[s.queue[i].track].aidx == a))
		i--;
	if (i >= 0) {   /* начало того альбома */
		int b = s.lib->tracks[s.queue[i].track].aidx;
		while (i > 0 && s.queue[i - 1].action == A_TRACK && s.lib->tracks[s.queue[i - 1].track].aidx == b)
			i--;
		play_queue(s.queue, s.qlen, i);
		player_release(-1);
		return;
	}
	if (switch_album(-1))
		player_release(-1);
}

/* свайп по карусели плеера: первый трек соседнего альбома */
static int switch_album(int dir)
{
	if (s.track < 0)
		return 0;
	int pod = s.lib->albums[s.lib->tracks[s.track].aidx].podcast;
	int a = s.album_pos[s.lib->tracks[s.track].aidx] + dir;
	if (a < (pod ? nmusic : 0) || a >= (pod ? s.lib->nalbums : nmusic))
		return 0;   /* музыка и подкасты листаются отдельно */
	const lib_album_t *al = &s.lib->albums[s.album_order[a]];
	play_queue(s.album_nodes[s.album_order[a]], al->ntracks, 0);
	return 1;
}

/* ---------- кадр ---------- */

/* бросок: позиция едет со скоростью kin_v, трение гасит её (время спада ~0.3 с: дальность ≈ скорость × 0.3);
 * у края — стоп; медленно — дальше обычное подтягивание к ближайшему пункту */
static void kinetic(double dt)
{
	if (!s.kin)
		return;
	if (s.kin == 2 && !s.opt_open)
		s.kin = 0;
	if (!s.kin)
		return;
	column_t *c = s.kin == 1 ? cur_col() : NULL;
	int grid = c && c->grid;
	int n = s.kin == 2 ? s.opt_count : grid ? (c->count + GRID_COLS - 1) / GRID_COLS : c->count;
	double *pos = s.kin == 2 ? &s.opt_pos : grid ? &c->rpos : &c->pos;
	*pos += s.kin_v * dt;
	s.kin_v *= exp(-dt / 0.3);
	if (*pos <= 0 || *pos >= n - 1) {
		*pos = fmax(0, fmin(n - 1, *pos));
		s.kin_v = 0;
	}
	int i = (int)lround(*pos);
	if (s.kin == 2)
		s.opt_sel = i;
	else if (grid)
		c->sel = (int)fmin(c->count - 1, i * GRID_COLS + c->sel % GRID_COLS);
	else
		c->sel = i;
	if (fabs(s.kin_v) < 3)
		s.kin = 0;   /* дальше — подтягивание к пункту */
}

static void bt_apply(void)
{
	bt_power(!s.bt_off);
	if (!s.bt_off)
		bt_scan();
}

static void bt_codec_apply(void)
{
	bt_set_codec(codec_bt[s.codec]);
}

/* пункты «Устройств» из снимка bt.c; открытый список обновляется на месте */
static void bt_build(void)
{
	int n = s.bt_off ? 0 : bt_devices(bt_list, BT_MAX), conn = -1;
	for (int i = 0; i < n; i++) {
		bt_nodes[i] = (node_t){ .icon = LV_SYMBOL_BLUETOOTH, .title = bt_list[i].name, .action = A_BTDEV, .track = i,
					.sub = bt_list[i].connected ? "подключено — клик отключит"
					       : bt_list[i].paired ? "сопряжено" : NULL };
		if (bt_list[i].connected && conn < 0)
			conn = i;
	}
	const char *st = bt_status();
	if (s.bt_off)
		bt_nodes[n] = (node_t){ .icon = LV_SYMBOL_BLUETOOTH, .title = "Bluetooth выключен", .sub = "включите его выше" };
	else
		bt_nodes[n] = (node_t){ .icon = LV_SYMBOL_REFRESH, .title = st[0] ? st : "Искать устройства",
					.sub = "наушники — в режим сопряжения", .action = A_BTSCAN };
	bt_items[1].nkids = n + 1;
	if (conn >= 0)
		snprintf(bt_dev_sub, sizeof(bt_dev_sub), "%s", bt_list[conn].name);
	else
		snprintf(bt_dev_sub, sizeof(bt_dev_sub), "%s", tx(n ? "не подключено" : "нет устройств"));
	for (int d = 1; d <= s.depth; d++) {
		column_t *c = &s.stack[d];
		if (c->nodes != bt_nodes)
			continue;
		int sel = c->sel;
		column_fill(c, bt_nodes, n + 1);
		c->sel = c->pos = sel <= n ? sel : n;
		layout();
	}
	refresh_subs();
}

static void wifi_apply(void)
{
	wifi_power(!s.wifi_off);
}

static void wifi_build(void)
{
	int n = s.wifi_off ? 0 : wifi_networks(wifi_list, WIFI_MAX), conn = -1;
	for (int i = 0; i < n; i++) {
		const wifi_net_t *w = &wifi_list[i];
		const char *sig = tx(w->signal >= -60 ? "сильный сигнал" : w->signal >= -72 ? "средний сигнал" : "слабый сигнал");
		snprintf(wifi_sub[i], sizeof(wifi_sub[i]), "%s · %s",
			 tx(w->connected ? "подключено" : w->saved ? "сохранена" : w->secured ? "с паролем" : "открытая"), sig);
		wifi_nodes[i] = (node_t){ .icon = LV_SYMBOL_WIFI, .title = w->ssid, .sub = wifi_sub[i],
					  .action = A_WIFINET, .track = i };
		if (w->connected)
			conn = i;
	}
	const char *st = wifi_status();
	if (s.wifi_off)
		wifi_nodes[n] = (node_t){ .icon = LV_SYMBOL_WIFI, .title = "Wi-Fi выключен", .sub = "включите его выше" };
	else
		wifi_nodes[n] = (node_t){ .icon = LV_SYMBOL_REFRESH, .title = st[0] ? st : "Искать сети", .action = A_WIFISCAN };
	wifi_items[1].nkids = n + 1;
	snprintf(wifi_items_sub, sizeof(wifi_items_sub), "%s",
		 conn >= 0 ? wifi_list[conn].ssid : tx(s.wifi_off ? "выключен" : "не подключено"));
	for (int d = 1; d <= s.depth; d++) {
		column_t *c = &s.stack[d];
		if (c->nodes != wifi_nodes)
			continue;
		int sel = c->sel;
		column_fill(c, wifi_nodes, n + 1);
		c->sel = c->pos = sel <= n ? sel : n;
		layout();
	}
	refresh_subs();
}

static void wifi_poll(void)
{
	static unsigned gen = ~0u, mseq;
	if (wifi_gen() != gen) {
		gen = wifi_gen();
		wifi_build();
	}
	unsigned seq;
	const char *m = wifi_message(&seq);
	if (seq != mseq) {
		mseq = seq;
		toast("%s", m);
	}
}

/* подписи «Подкастов»: сколько их и что сейчас с подписками */
static void pod_labels(void)
{
	int npod = music[MUSIC_PODCASTS].nkids - 1;
	if (npod > 0)
		snprintf(pod_sub, sizeof(pod_sub), tx("подкастов: %d"), npod);
	else
		snprintf(pod_sub, sizeof(pod_sub), "%s", tx(pod_feeds() ? "выпусков пока нет" : "подписки — в Podcasts/подписки.txt"));
	const char *st = pod_status();
	snprintf(pod_upd_sub, sizeof(pod_upd_sub), "%s",
		 st[0] ? st : tx(!pod_feeds() ? "ссылки на RSS — в Podcasts/подписки.txt" : wifi_online() ? "проверить новые выпуски"
											  : "нужен Wi-Fi"));
}

/* где остановился в выпуске: раз в 10 с, при смене трека — и «прослушан», если дошёл почти до конца. Новые
 * выпуски скачались — библиотека перечитывается перезапуском XMB, когда ничего не играет */
static void podcast_poll(void)
{
	static int pt = -1, pending;
	static double ppos, pdur;
	static uint32_t pms, lms;
	static unsigned mseq;
	static char last_st[96];
	uint32_t now = lv_tick_get();
	if (!s.lib || cats[CAT_MUSIC].items != music)
		return;
	pod_tick(wifi_online());
	if (strcmp(last_st, pod_status()) || now - lms > 5000) {
		snprintf(last_st, sizeof(last_st), "%s", pod_status());
		lms = now;
		pod_labels();
		refresh_subs();
	}
	unsigned seq;
	const char *m = pod_message(&seq);
	if (seq != mseq) {
		mseq = seq;
		if (s.screen_on)
			toast("%s", m);
	}
	int cur = s.track >= 0 && !s.resume && s.lib->albums[s.lib->tracks[s.track].aidx].podcast ? s.track : -1;
	if (pt >= 0 && pt != cur)
		pod_save(s.lib->tracks[pt].path, ppos, ppos >= pdur - 30);
	if (cur >= 0) {
		if (cur != pt)
			pms = now;
		ppos = s.pos;
		pdur = s.dur;
		if (now - pms > 10000) {
			pms = now;
			pod_save(s.lib->tracks[cur].path, ppos, ppos >= pdur - 30);
		}
	}
	pt = cur;
	pending += pod_take_new();
	if (pending && !s.playing && !s.kb_panel && !s.opt_open) {
		pending = 0;
		restart_req = 1;   /* перечитать библиотеку: место в меню восстановится */
	}
}

static void bt_poll(void)
{
	static unsigned gen = ~0u, mseq;
	if (bt_gen() != gen) {
		gen = bt_gen();
		bt_build();
	}
	unsigned seq;
	const char *m = bt_message(&seq);
	if (seq != mseq) {
		mseq = seq;
		toast("%s", m);
	}
}

static void frame_cb(lv_timer_t *t)
{
	(void)t;
	uint32_t now = lv_tick_get();
	double dt = (now - s.last_ms) / 1000.0;
	s.last_ms = now;
	if (dt > 0.1)
		dt = 0.1;

	if (s.t_mode != T_HCAT)
		s.cat_pos = approach(s.cat_pos, s.cat, dt, 14);
	s.depth_t = approach(s.depth_t, s.depth, dt, 12);
	s.slide = approach(s.slide, 0, dt, 14);
	s.focus_t = approach(s.focus_t, s.at_cats, dt, 12);
	kinetic(dt);
	for (int i = 0; i < MAX_DEPTH; i++) {
		column_t *c = col_at(i);
		int held = (s.t_mode == T_VLIST && !s.opt_open && i == s.depth) || (s.kin == 1 && i == s.depth);
		if (!held)
			c->pos = approach(c->pos, c->sel, dt, 16 + 10 * fmin(fabs(c->sel - c->pos), 4));
		if (c->grid && !held) {
			double tr = c->sel / GRID_COLS;
			c->rpos = approach(c->rpos, tr, dt, 14 + 8 * fmin(fabs(tr - c->rpos), 4));
		}
	}
	if (!(s.t_mode == T_VLIST && s.opt_open) && s.kin != 2)
		s.opt_pos = approach(s.opt_pos, s.opt_sel, dt, 16);
	s.opt_t = approach(s.opt_t, s.opt_open, dt, 14);
	s.opt_sub = approach(s.opt_sub, 0, dt, 12);
	if (!s.opt_open && s.opt_t == 0)
		s.opt_item = NULL;
	if (s.opt_open || s.opt_t > 0)
		layout_options();   /* панель вариантов лежит поверх плеера, вне общего меню */
	if (s.opt_open && s.t_mode == T_VLIST)
		options_preview(s.opt_sel);   /* предпросмотр и при листании пальцем */
	if (s.t_mode != T_PULL && s.t_mode != T_SHADE)
		s.sh_t = approach(s.sh_t, s.sh_open, dt, 14);
	if (s.sh_t > 0 || s.sh_open)
		layout_shade();   /* поверх и меню, и плеера */

	slide_tick(dt);
	if (s.bhint_t > 0) {   /* подсказка кнопки: уходит вверх, к своему краю, как боковые — к своим, и гаснет */
		s.bhint_t = fmax(0, s.bhint_t - dt / 0.7);
		int w = lv_obj_get_width(s.bhint);
		set_pos(s.bhint, (SCR_W - w) / 2, TOP_H + 14 - (int)((1.0 - s.bhint_t) * 14));
		lv_obj_set_style_opa(s.bhint, (lv_opa_t)(230 * fmin(1.0, s.bhint_t * 2)), 0);
		set_hidden(s.bhint, s.bhint_t == 0);
	}
	for (int i = 0; i < 2; i++) {
		if (s.hint_t[i] > 0) {
			s.hint_t[i] = fmax(0, s.hint_t[i] - dt / 0.5);
			int dx = (int)((1.0 - s.hint_t[i]) * 14) * (i ? 1 : -1);
			set_pos(s.hint[i], (i ? SCR_W - 60 : 16) + dx, SCR_H / 2 - 22);
			lv_obj_set_style_opa(s.hint[i], (lv_opa_t)(220 * s.hint_t[i]), 0);
			set_hidden(s.hint[i], s.hint_t[i] == 0);
		}
	}

	library_poll();
	settings_poll();
	bt_poll();
	wifi_poll();
	lyrics_poll();
	podcast_poll();
	battery_guard();
	/* гашение экрана без касаний и кнопок; пока тянут шторку или ползунок — не гаснет */
	if (s.screen_on && screen_sec[s.screen_off] && s.t_mode == T_NONE &&
	    now - s.input_ms > (uint32_t)screen_sec[s.screen_off] * 1000u)
		screen(0);
	/* таймер сна: последние 15 с громкость уходит в ноль, затем пауза, громкость возвращается, экран гаснет */
	if (s.sleep_end) {
		int32_t left = (int32_t)(s.sleep_end - now);
		static int last_sec = -1;
		if (left / 1000 != last_sec) {
			last_sec = left / 1000;
			update_modes();
		}
		if (left <= 0) {
			s.sleep_end = 0;
			s.sleep = SLEEP_OFF;
			if (s.playing)
				toggle_play();
			audio_set_volume(s.volume);
			update_modes();
			refresh_subs();
			if (s.screen_on)
				screen(0);
		} else if (left < SLEEP_FADE_MS && s.playing) {
			audio_set_volume(s.volume * left / SLEEP_FADE_MS);
		}
	}
	if (s.toast_t > 0) {   /* уведомление — как значок кнопки: сверху по центру, в конце уходит вверх и гаснет */
		s.toast_t = fmax(0, s.toast_t - dt / 1.8);
		double out = clamp01((0.25 - s.toast_t) / 0.25);
		lv_obj_align(s.toast, LV_ALIGN_TOP_MID, 0, TOP_H + 14 - (int)(out * 14));
		lv_obj_set_style_opa(s.toast, (lv_opa_t)(230 * (1.0 - out)), 0);
		set_hidden(s.toast, s.toast_t == 0);
	}
	if (s.bubble_t > 0) {   /* буква указателя: держится, затем гаснет */
		s.bubble_t = fmax(0, s.bubble_t - dt / 0.9);
		lv_obj_set_style_opa(s.bubble, (lv_opa_t)(255 * fmin(1.0, s.bubble_t * 2.5)), 0);
		set_hidden(s.bubble, s.bubble_t == 0);
	}
	audio_status_t st;
	audio_get_status(&st);
	if (s.ab == 2 && s.playing && s.pos >= s.ab_b) {   /* повтор A-B: дошли до B — к A */
		seek_to(s.ab_a);
		s.pos = s.ab_a;
	}
	if (s.playing || st.playing)
		s.active_ms = now;   /* последний момент, когда играла музыка: глубокий сон — только после долгой тишины */
	if (s.playing && now - s.last_saved_ms > 30000)
		last_save_later();
	if (q_dirty && now - q_dirty > 2000) {
		q_dirty = 0;
		q_write();
	}
	if (last_dirty && now - last_dirty > 2000) {
		last_dirty = 0;
		last_save();
	}
	if (s.track >= 0 && !s.resume) {
		if (st.gen != s.audio_gen) {   /* поток открыл заказанный трек: формат — его */
			s.audio_gen = st.gen;
			if (st.bits && st.rate)
				snprintf(s.np_format, sizeof(s.np_format), "%s · %d bit / %g kHz", st.codec, st.bits, st.rate / 1000.0);
			else
				snprintf(s.np_format, sizeof(s.np_format), "%s", st.codec);
			update_np_labels();
		}
		if (st.seq != s.seq) {   /* движок сам перешёл на заказанный следующий */
			s.seq = st.seq;
			if (s.next_qi >= 0)
				np_switch(s.queue, s.qlen, s.next_qi);
		} else if (st.ended && !s.ended_seen) {
			s.ended_seen = 1;
			next_track(1, 1);   /* статус этого кадра — ещё от старого трека: не применять */
		} else {
			if (st.playing != s.playing) {
				s.playing = st.playing;
				update_np_labels();
			}
			if (s.seek_ms && (fabs(st.pos - s.seek_hold) < 1.0 || lv_tick_get() - s.seek_ms > 600))
				s.seek_ms = 0;
			if (!s.seek_ms)
				s.pos = st.pos;
			if (st.dur > 0)
				s.dur = st.dur;
		}
	}
	if (s.playing && s.dur > 0) {
		int w = (int)(s.mini_bar_w * s.pos / s.dur);
		if (lv_obj_get_width(s.mini_bar) != w)
			lv_obj_set_width(s.mini_bar, w);
		static int last_sec = -1;
		if ((int)s.pos != last_sec) {
			last_sec = (int)s.pos;
			lv_label_set_text_fmt(s.mini_time, "%d:%02d", last_sec / 60, last_sec % 60);
		}
		player_set_state(s.playing, s.seek_preview >= 0 ? s.seek_preview : s.pos, s.volume);
	}

	/* визуализатор: уровень сигнала (RMS), быстрая атака, медленный спад */
	double lvl = s.playing ? fmin(1.0, st.level * 3.5) : 0;
	s.level = lvl > s.level ? approach(s.level, lvl, dt, 30) : approach(s.level, lvl, dt, 5);
	if (s.track >= 0)
		mini_cover();   /* опрос держит обложку в кэше и подхватывает готовую */
	bg_set_level(s.level);
	bg_set_band(player_progress() > 0.5);
	/* спектр: быстрая атака, медленный спад — полосы не дёргаются, но успевают за ударными */
	static float sp[AUDIO_BANDS];
	for (int i = 0; i < AUDIO_BANDS; i++) {
		float v = s.playing ? st.spec[i] : 0;
		sp[i] = v > sp[i] ? sp[i] + (v - sp[i]) * (float)fmin(1.0, dt * 28) : sp[i] + (v - sp[i]) * (float)fmin(1.0, dt * 5);
	}
	bg_set_spectrum(sp, AUDIO_BANDS);

	if (s.screen_on) {
		static int prof = -1;   /* замеры (XMB_SCRIPT): долгие раскладка и волна — в журнал */
		if (prof < 0)
			prof = getenv("XMB_SCRIPT") != NULL;
		uint32_t t0 = lv_tick_get();
		player_tick(dt);
		if (!player_covers())
			layout();
		uint32_t t1 = lv_tick_get();
		if (!player_carousel_moving())   /* полоса обложек и волна вместе не укладываются в кадр — волна ждёт */
			bg_tick(dt);
		update_topbar();
		activity_tick();
		uint32_t t2 = lv_tick_get();
		if (prof && t2 - t0 > 8)
			printf("кадр логики: раскладка %u мс, волна и верх %u мс\n", t1 - t0, t2 - t1);
	}
	set_hidden(s.main, player_covers());
	/* под плеером — затемнение или цвет обложки (гуще: краска вместо черноты). Смена обложки — плавно, но фон
	 * пересчитывается целиком (весь экран), поэтому шаг — не чаще раза в 60 мс */
	uint32_t tint = s.player_bg ? player_tint() : 0;
	double want[4] = { (tint >> 16) & 255, (tint >> 8) & 255, tint & 255, tint ? 215 : PLAYER_DIM };
	static double cur[4];
	static uint32_t tint_ms;
	if (!player_covers()) {
		memcpy(cur, want, sizeof(cur));   /* плеер открывается сразу в своём цвете */
		bg_set_dim(0, tint);
	} else if (lv_tick_get() - tint_ms >= 60) {
		double k = 1.0 - exp(-(double)(lv_tick_get() - tint_ms) / 1000.0 / 0.18);
		tint_ms = lv_tick_get();
		for (int i = 0; i < 4; i++)
			cur[i] = fabs(want[i] - cur[i]) < 3 ? want[i] : cur[i] + (want[i] - cur[i]) * fmin(1.0, k);
		/* RGB565: младшие биты цвета не видны — без них не пересчитывать фон зря */
		uint32_t c = ((uint32_t)cur[0] << 16 | (uint32_t)cur[1] << 8 | (uint32_t)cur[2]) & 0xF8FCF8;
		bg_set_dim((int)cur[3], c);
	}
	set_hidden(s.mini, player_progress() > 0);
	static int covered = -1;
	if (covered != (player_progress() > 0)) {
		covered = player_progress() > 0;
		corners_place();
	}
	/* плеер свернулся — содержимое мини-плеера проявляется, а не возникает за кадр. Фон мини-плеера не трогаем:
	 * он такой же, как у свернувшегося плеера */
	static double mini_t = 1;
	if (player_progress() > 0) {
		mini_t = 0;
	} else if (mini_t < 1) {
		mini_t = fmin(1.0, mini_t + dt / 0.15);
		lv_opa_t o = mini_t >= 1 ? LV_OPA_COVER : (lv_opa_t)(255 * mini_t);
		for (uint32_t i = 0; i < lv_obj_get_child_count(s.mini); i++)
			lv_obj_set_style_opa(lv_obj_get_child(s.mini, (int32_t)i), o, 0);
	}
}

/* ---------- действия ---------- */

static void apply_theme(void)
{
	bg_set_colors(theme_colors[s.theme][0], theme_colors[s.theme][1]);
	apply_mini();
}

/* вид мини-плеера: насколько он закрывает строки списка, которые уходят под него */
static void apply_mini(void)
{
	if (!s.mini)
		return;
	uint32_t c = theme_colors[s.theme][1];   /* низ фона темы, притемнённый: мини-плеер как часть фона */
	uint32_t dark = ((c >> 16 & 255) * 2 / 5) << 16 | ((c >> 8 & 255) * 2 / 5) << 8 | (c & 255) * 2 / 5;
	int card = s.mini_style == MINI_CARD;
	uint32_t color = s.mini_style == MINI_THEME || card ? dark : 0;
	int opa = s.mini_style == MINI_CLEAR ? 77 : s.mini_style == MINI_DARK ? 217 : 255;
	int x = card ? 6 : 0, w = SCR_W - 2 * x, h = card ? MINI_H - 4 : MINI_H, inset = card ? 12 : 0;
	lv_obj_set_pos(s.mini, x, SCR_H - MINI_H);
	lv_obj_set_size(s.mini, w, h);
	lv_obj_set_style_radius(s.mini, card ? 12 : 0, 0);
	lv_obj_set_style_bg_color(s.mini, lv_color_hex(color), 0);
	lv_obj_set_style_bg_opa(s.mini, (lv_opa_t)opa, 0);
	/* у карточки полоса прогресса — не до краёв: углы скруглены; у «Чёрного со скруглением» — внизу, под текстом:
	 * сверху она разделяла бы мини-плеер и скругления над ним */
	int frame = s.mini_style == MINI_FRAME, bar_y = frame ? h - 5 : 0;
	if (frame)
		inset = 10;
	s.mini_bar_w = w - 2 * inset;
	lv_obj_set_pos(s.mini_track, inset, bar_y);
	lv_obj_set_width(s.mini_track, s.mini_bar_w);
	lv_obj_set_pos(s.mini_bar, inset, bar_y);
	lv_obj_set_x(s.mini_time, w - 54);
	lv_obj_set_x(s.mini_icon, w - 54);
	player_set_mini_bg(color, opa);
	corners_place();
}

static void spectrum_update(void);

static void apply_wave(void)
{
	bg_set_wave((bg_wave_t)s.wave);
	spectrum_update();
}

static void apply_gain(void)
{
	audio_set_gain(s.gain);
}

static void apply_rg(void)
{
	float db, peak;
	track_rg(s.track, &db, &peak);
	audio_set_replaygain(db, peak);
	if (s.next_qi >= 0)
		plan_next();
}

/* язык: строки списков привязываются заново, подписи плеера, шторки, эквалайзера — обновляются */
static void apply_lang(void)
{
	i18n_set(s.lang);
	for (int d = 0; d < MAX_DEPTH; d++) {
		column_t *c = col_at(d);
		for (int i = 0; i < POOL && c->pool_ready; i++)
			c->rows[i].idx = -1;
	}
	for (int c = 0; c < NCAT; c++)
		for (int i = 0; i < POOL && s.cat_col[c].pool_ready; i++)
			s.cat_col[c].rows[i].idx = -1;
	lv_label_set_text(s.cat_title, tx(cats[s.cat].title));
	for (int i = 0; i < NTILE; i++)
		lv_label_set_text(s.sh_label[i], tx(tiles[i].name));
	lv_label_set_text(s.eq_title, tx("Эквалайзер"));
	if (s.opt_open && s.opt_item)
		for (int i = 0; i < s.opt_count; i++)
			lv_label_set_text(s.opt_rows[i], tx(s.opt_item->opts[i]));
	update_np_labels();
	update_modes();
	refresh_subs();
	layout();
}

static void apply_bright(void)
{
	platform_brightness((s.bright + 1) * 10);
}

static void options_open(const node_t *it)
{
	/* ширина — по самой длинной строке (в пределах OPT_W); у тем — только цветные образцы, меню узкое */
	int w = 0;
	for (int i = 0; !it->swatch && it->opts[i] && i < MAX_OPTS; i++) {
		lv_point_t sz;
		lv_text_get_size(&sz, tx(it->opts[i]), s.f_item, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
		w = sz.x > w ? sz.x : w;
	}
	s.opt_w = it->swatch ? 96 : w + 24 + 46;
	s.opt_w = s.opt_w < 120 && !it->swatch ? 120 : s.opt_w > OPT_W ? OPT_W : s.opt_w;
	s.opt_sub = s.opt_t > 0.5 ? 1 : 0;   /* открыто поверх уже видного меню (действия → «В плейлист…») — вглубь */
	lv_obj_set_width(s.opt_panel, s.opt_w);
	lv_obj_clean(s.opt_panel);
	lv_obj_t *hl = plain(s.opt_panel);
	lv_obj_set_size(hl, s.opt_w - 16, 34);
	lv_obj_set_pos(hl, 8, SCR_H / 2 - 17);
	lv_obj_set_style_radius(hl, 8, 0);
	lv_obj_set_style_bg_color(hl, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(hl, 38, 0);
	s.opt_item = it;
	s.opt_count = 0;
	for (int i = 0; it->opts[i] && i < MAX_OPTS; i++) {
		s.opt_rows[i] = label(s.opt_panel, s.f_item, tx(it->opts[i]));
		lv_obj_set_size(s.opt_rows[i], s.opt_w - 60, 24);
		lv_label_set_long_mode(s.opt_rows[i], LV_LABEL_LONG_MODE_DOTS);
		s.opt_checks[i] = label(s.opt_panel, &lv_font_montserrat_20, LV_SYMBOL_OK);
		s.opt_sw[i] = NULL;
		if (it->swatch) {
			/* образец темы: её градиент сверху вниз */
			lv_obj_t *sw = plain(s.opt_panel);
			lv_obj_set_size(sw, 40, 28);
			lv_obj_set_style_radius(sw, 5, 0);
			lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, 0);
			lv_obj_set_style_bg_color(sw, lv_color_hex(it->swatch[i][0]), 0);
			lv_obj_set_style_bg_grad_color(sw, lv_color_hex(it->swatch[i][1]), 0);
			lv_obj_set_style_bg_grad_dir(sw, LV_GRAD_DIR_VER, 0);
			lv_obj_set_style_border_color(sw, lv_color_white(), 0);
			lv_obj_set_style_border_opa(sw, LV_OPA_40, 0);
			lv_obj_set_style_border_width(sw, 1, 0);
			s.opt_sw[i] = sw;
		}
		s.opt_count++;
	}
	s.opt_sel = *it->val;
	s.opt_orig = *it->val;
	s.opt_pos = s.opt_sel;
	s.opt_open = 1;
	layout_options();
}

/* выбранный в списке вариант применяется сразу (тема, фон, яркость видны без клика) */
static void options_preview(int v)
{
	if (s.opt_item && s.opt_item->apply && *s.opt_item->val != v) {
		*s.opt_item->val = v;
		s.opt_item->apply();
	}
}

static void options_cancel(void)
{
	options_preview(s.opt_orig);
	s.opt_open = 0;
	refresh_subs();
}

static void options_apply(void)
{
	*s.opt_item->val = s.opt_sel;
	if (s.opt_item->apply)
		s.opt_item->apply();
	s.opt_open = 0;
	if (s.opt_item->commit)
		s.opt_item->commit();
	refresh_subs();
}

static void set_cat(int c)
{
	if (c < 0 || c >= NCAT || c == s.cat)
		return;
	for (int d = 1; d <= s.depth; d++)
		hide_column(&s.stack[d]);
	hide_column(col_at(0));
	s.depth = 0;
	s.depth_t = 0;
	s.slide = c > s.cat ? 1 : -1;   /* новый список въезжает с той стороны, куда идёт ряд */
	s.cat = c;
	col_at(0)->pos = col_at(0)->sel;
	lv_label_set_text(s.cat_title, tx(cats[c].title));
	refresh_subs();
	layout();
}

/* подсказки — на верхнем слое, а окна (эквалайзер, клавиатура, ползунок) создаются там позже и легли бы сверху */
static void show_hint(int i)
{
	s.hint_t[i] = 1.0;
	lv_obj_move_foreground(s.hint[i]);
}

/* что сделала кнопка на корпусе — значком сверху по центру (как «назад» и «выбрать» у тапа) */
static void show_btn_hint(const char *glyph, const lv_font_t *f)
{
	if (!s.screen_on || !glyph || s.toast_t > 0.97)   /* нажатие уже ответило текстом (повтор, случайно) */
		return;
	s.toast_t = 0;   /* место сверху одно: показано последнее */
	set_hidden(s.toast, true);
	lv_obj_move_foreground(s.bhint);
	lv_obj_set_style_text_font(s.bhint_lbl, f, 0);
	lv_label_set_text(s.bhint_lbl, glyph);
	lv_point_t sz;
	lv_text_get_size(&sz, glyph, f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
	lv_obj_set_width(s.bhint, sz.x + 26 > 44 ? sz.x + 26 : 44);
	/* треугольник «играть» выровнен по рамке знака, а глаз центрирует его по массе — она левее на треть */
	lv_obj_align(s.bhint_lbl, LV_ALIGN_CENTER, !strcmp(glyph, LV_SYMBOL_PLAY) ? 3 : 0, 0);
	s.bhint_t = 1.0;
}

static void shade_set(int open)
{
	s.sh_open = open;
	s.sh_drag = 0;
	if (open && s.opt_open)
		options_cancel();
}

static void usb_menu(void);

static void tile_tap(int i)
{
	switch (i) {
	case TL_BT:
		s.bt_off = !s.bt_off;
		bt_apply();
		break;
	case TL_WIFI:
		s.wifi_off = !s.wifi_off;
		wifi_apply();
		break;
	case TL_EQ:
		s.eq_on = !s.eq_on;
		eq_apply();
		break;
	case TL_SLEEP:   /* выключен → 10 → 30 минут → выключен */
		s.sleep = s.sleep == SLEEP_OFF ? 2 : s.sleep == 2 ? 3 : SLEEP_OFF;
		sleep_start();
		if (s.sleep != SLEEP_OFF)
			toast("Сон через %d мин", sleep_min[s.sleep]);
		break;
	case TL_SHUFFLE: toggle_mode(PM_SHUFFLE); break;
	case TL_REPEAT: toggle_mode(PM_REPEAT); break;
	case TL_TOUCH: s.touch_off = !s.touch_off; break;
	case TL_USB:
		shade_set(0);
		usb_menu();
		break;
	}
	refresh_subs();
}

/* громкость колесом — по делениям шкалы: щелчок — ровно одно деление (к следующему, даже если стоит между ними) */
static int volume_seg(int v, int dir)
{
	for (int k = dir > 0 ? 0 : SEG; k >= 0 && k <= SEG; k += dir > 0 ? 1 : -1) {
		int kv = (k * 100 + SEG / 2) / SEG;
		if (dir > 0 ? kv > v : kv < v)
			return kv;
	}
	return dir > 0 ? 100 : 0;
}

/* ползунок шторки по касанию: x — точка на дорожке; громкость — по делениям, как колесом */
static void shade_slider(int i, int x)
{
	int k = ((x - SLIDER_X) * (i ? SEG : 10) + SLIDER_W / 2) / SLIDER_W;
	if (i == 0) {
		s.bright = k - 1 < 0 ? 0 : k - 1 > 9 ? 9 : k - 1;
		apply_bright();
	} else {
		k = k < 0 ? 0 : k > SEG ? SEG : k;
		s.volume = (k * 100 + SEG / 2) / SEG;
		audio_set_volume(s.volume);
		player_set_state(s.playing, s.pos, s.volume);
	}
	lv_obj_invalidate(s.sh_slider[i]);
	refresh_subs();
}

/* ---------- «прокрутите, чтобы…»: подтверждение колесом, как slide to unlock, но вертикально ----------
 * Для редких действий, которые меняют режим или необратимы: щелчки колеса в одну сторону двигают ползунок по
 * дорожке, дошёл до конца — действие; перестали крутить — пружиной обратно; пальцем — тянуть ползунок. Случайное
 * нажатие в кармане так не сработает, а прогресс виден */
#define SL_STEPS 5       /* щелчков колеса до конца */
#define SL_W 60          /* дорожка */
#define SL_H 196
#define SL_KNOB 50
#define SL_PAD 5
#define SL_TOP 168       /* верх дорожки в панели */

static struct {
	lv_obj_t *panel, *track, *fill, *knob, *knob_icon, *hint, *shine, *shine_lbl, *cancel;
	int steps, dir, dragging, finishing;
	double pos, shine_t;
	uint32_t last_ms;
	void (*done)(void), (*cancelled)(void);
} sl;

static void slide_close(void)
{
	if (!sl.panel)
		return;
	lv_obj_delete(sl.panel);
	memset(&sl, 0, sizeof(sl));
}

static void slide_cancel(void)
{
	void (*cb)(void) = sl.cancelled;
	slide_close();
	if (cb)
		cb();
}

static void slide_cancel_cb(lv_event_t *e)
{
	(void)e;
	slide_cancel();
}

/* палец на дорожке: ползунок — под пальцем */
static void slide_drag_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	if (code == LV_EVENT_PRESSING) {
		lv_point_t p;
		lv_indev_get_point(lv_indev_active(), &p);
		lv_area_t a;
		lv_obj_get_coords(sl.track, &a);
		double k = clamp01((p.y - a.y1 - SL_PAD - SL_KNOB / 2.0) / (SL_H - 2 * SL_PAD - SL_KNOB));
		sl.steps = (int)lround((sl.dir > 0 ? k : 1 - k) * SL_STEPS);
		sl.dragging = 1;
		sl.last_ms = lv_tick_get();
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		sl.dragging = 0;
		sl.last_ms = 0;   /* не дотянул — сразу обратно */
	}
}

/* dir: 1 — крутить вниз (ползунок сверху вниз), −1 — вверх. cancel_text — NULL: отмены нет */
static void slide_open(const char *icon, const char *title, const char *sub, const char *hint, int dir,
		       const char *cancel_text, void (*done)(void), void (*cancelled)(void))
{
	slide_close();
	sl.dir = dir;
	sl.done = done;
	sl.cancelled = cancelled;
	sl.panel = plain(lv_layer_top());
	lv_obj_set_size(sl.panel, SCR_W, SCR_H - TOP_H);
	lv_obj_set_pos(sl.panel, 0, TOP_H);
	lv_obj_set_style_bg_color(sl.panel, lv_color_hex(0x101114), 0);   /* глухой: меню под ним не просвечивает */
	lv_obj_set_style_bg_opa(sl.panel, LV_OPA_COVER, 0);
	lv_obj_set_clickable(sl.panel, true);   /* касания не проходят в меню под панелью */

	lv_obj_t *ic = label(sl.panel, &lv_font_montserrat_40, icon);
	lv_obj_align(ic, LV_ALIGN_TOP_MID, 0, 26);
	lv_obj_t *t1 = label(sl.panel, s.f_title, tx(title));
	lv_obj_set_width(t1, SCR_W - 24);
	lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_align(t1, LV_ALIGN_TOP_MID, 0, 84);
	lv_obj_t *t2 = label(sl.panel, s.f_item, sub);   /* sub — уже переведённый: в нём бывает название */
	lv_obj_set_width(t2, SCR_W - 32);
	lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_opa(t2, 180, 0);
	lv_obj_align(t2, LV_ALIGN_TOP_MID, 0, 118);

	sl.track = plain(sl.panel);
	lv_obj_set_size(sl.track, SL_W, SL_H);
	lv_obj_set_pos(sl.track, (SCR_W - SL_W) / 2, SL_TOP);
	lv_obj_set_style_radius(sl.track, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(sl.track, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(sl.track, 40, 0);
	lv_obj_set_clickable(sl.track, true);
	lv_obj_add_event_cb(sl.track, slide_drag_cb, LV_EVENT_ALL, NULL);
	sl.fill = plain(sl.track);   /* пройденная часть дорожки */
	lv_obj_set_style_radius(sl.fill, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(sl.fill, lv_color_hex(ACCENT), 0);
	lv_obj_set_style_bg_opa(sl.fill, 170, 0);
	sl.knob = plain(sl.track);
	lv_obj_set_size(sl.knob, SL_KNOB, SL_KNOB);
	lv_obj_set_style_radius(sl.knob, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(sl.knob, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(sl.knob, LV_OPA_COVER, 0);
	sl.knob_icon = label(sl.knob, &lv_font_montserrat_20, dir > 0 ? LV_SYMBOL_DOWN : LV_SYMBOL_UP);
	lv_obj_set_style_text_color(sl.knob_icon, lv_color_hex(0x30323a), 0);
	lv_obj_center(sl.knob_icon);

	/* подсказка с бегущим бликом: тусклая надпись и яркая её копия в узком окне, окно едет по строке.
	 * «Прокрутите вниз — удалить»: направление и действие */
	char hb[96];
	snprintf(hb, sizeof(hb), "%s — %s", tx(dir > 0 ? "Прокрутите вниз" : "Прокрутите вверх"), tx(hint));
	int hy = SL_TOP + SL_H + 16;
	sl.hint = label(sl.panel, s.f_item, hb);
	lv_obj_set_width(sl.hint, SCR_W);
	lv_obj_set_style_text_align(sl.hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_opa(sl.hint, 120, 0);
	lv_obj_set_pos(sl.hint, 0, hy);
	sl.shine = plain(sl.panel);
	lv_obj_set_size(sl.shine, 46, lv_font_get_line_height(s.f_item) + 4);
	lv_obj_set_pos(sl.shine, -46, hy);
	sl.shine_lbl = label(sl.shine, s.f_item, hb);
	lv_obj_set_width(sl.shine_lbl, SCR_W);
	lv_obj_set_style_text_align(sl.shine_lbl, LV_TEXT_ALIGN_CENTER, 0);

	if (cancel_text) {   /* отмена — тап по строке внизу (клик колесом — тоже) */
		sl.cancel = label(sl.panel, s.f_item, tx(cancel_text));
		lv_obj_set_style_text_opa(sl.cancel, 170, 0);
		lv_obj_set_style_pad_all(sl.cancel, 10, 0);
		lv_obj_align(sl.cancel, LV_ALIGN_BOTTOM_MID, 0, -6);
		lv_obj_set_clickable(sl.cancel, true);
		lv_obj_add_event_cb(sl.cancel, slide_cancel_cb, LV_EVENT_CLICKED, NULL);
	}
	sl.last_ms = lv_tick_get();
}

/* колесо и кнопки, пока открыт ползунок; 1 — ввод его */
static int slide_input(xmb_input_t in)
{
	if (!sl.panel)
		return 0;
	if (in == XMB_WHEEL_DOWN || in == XMB_WHEEL_UP) {
		int d = in == XMB_WHEEL_DOWN ? 1 : -1;
		if (!sl.finishing) {
			sl.steps += d == sl.dir ? 1 : -1;
			sl.steps = sl.steps < 0 ? 0 : sl.steps > SL_STEPS ? SL_STEPS : sl.steps;
		}
		sl.last_ms = lv_tick_get();
		return 1;
	}
	if ((in == XMB_SELECT || in == XMB_BACK) && sl.cancel) {
		slide_cancel();
		return 1;
	}
	/* кнопки воспроизведения работают как обычно, остальное под панелью не трогаем */
	return in != XMB_PLAY && in != XMB_NEXT_TRACK && in != XMB_PREV_TRACK;
}

static void slide_tick(double dt)
{
	if (!sl.panel)
		return;
	uint32_t now = lv_tick_get();
	if (sl.steps >= SL_STEPS)
		sl.finishing = 1;
	else if (!sl.dragging && sl.steps > 0 && now - sl.last_ms > 1000)
		sl.steps = 0;   /* перестали крутить — пружиной в начало */
	sl.pos = approach(sl.pos, (double)sl.steps / SL_STEPS, dt, sl.dragging ? 30 : 14);
	int travel = SL_H - 2 * SL_PAD - SL_KNOB;
	int ky = SL_PAD + (int)((sl.dir > 0 ? sl.pos : 1 - sl.pos) * travel);
	set_pos(sl.knob, (SL_W - SL_KNOB) / 2, ky);
	/* заливка — от начала дорожки до ползунка */
	int f0 = sl.dir > 0 ? SL_PAD : ky, f1 = sl.dir > 0 ? ky + SL_KNOB : SL_H - SL_PAD;
	if (lv_obj_get_y(sl.fill) != f0 || lv_obj_get_height(sl.fill) != f1 - f0) {
		lv_obj_set_pos(sl.fill, SL_PAD, f0);
		lv_obj_set_size(sl.fill, SL_W - 2 * SL_PAD, f1 - f0);
	}
	set_hidden(sl.fill, sl.pos < 0.02);
	/* блик: проход по строке за 1.6 с, пауза 0.6 с */
	sl.shine_t = fmod(sl.shine_t + dt, 2.2);
	int sx = (int)(-46 + (SCR_W + 46) * fmin(1.0, sl.shine_t / 1.6));
	set_pos(sl.shine, sx, lv_obj_get_y(sl.hint));
	set_pos(sl.shine_lbl, -sx, 0);
	if (sl.finishing && sl.pos > 0.98) {
		void (*cb)(void) = sl.done;
		slide_close();
		if (cb)
			cb();
	}
}

/* ---------- USB-накопитель: карта отдаётся компьютеру, плеер ждёт на экране «USB» ---------- */

static struct {
	int host, req, end, storage, choice;
} usb;
static const char *const usb_opts[] = { "Отдать карту", "Только зарядка", NULL };

static void usb_commit(void)
{
	if (usb.choice == 0)
		usb.req = 1;
}

static const node_t usb_node = { .icon = LV_SYMBOL_USB, .title = "USB", .opts = usb_opts, .val = &usb.choice,
				 .commit = usb_commit };

static void usb_menu(void)
{
	if (usb.host)
		xmb_usb_host(1, 1);   /* тот же выбор, что при подключении кабеля */
	else
		toast("%s", "Подключите кабель к компьютеру");
}

static void usb_return(void)
{
	usb.end = 1;
}

/* кабель к компьютеру: отдать карту или только заряжаться — обычным выбором (вход безопасен: вернуть карту
 * можно в любой момент); возврат карты — ползунком */
void xmb_usb_host(int connected, int offer)
{
	usb.host = connected;
	if (connected && offer && !usb.storage && s.screen_on && !s.kb_panel) {
		if (s.opt_open)
			options_cancel();
		usb.choice = 0;
		options_open(&usb_node);
	} else if (!connected && s.opt_open && s.opt_item == &usb_node) {
		options_cancel();
	}
}

bool xmb_usb_storage_requested(void)
{
	int r = usb.req;
	usb.req = 0;
	return r;
}

bool xmb_usb_end_requested(void)
{
	int r = usb.end;
	usb.end = 0;
	return r;
}

static void settings_poll(void);

/* перед отдачей карты: пауза, позиция, очередь, настройки — на карту; файлы закрыты, обложки не читаются;
 * дальше на карту ничего не пишется (путь настроек сброшен) */
void xmb_storage_prepare(void)
{
	if (s.playing)
		toggle_play();
	save_flush();
	s.settings_ms = 0;
	settings_poll();
	fsave_flush();
	audio_release();
	cover_hold(1);
	s.settings_path[0] = 0;
	usb.storage = 1;
	if (s.opt_open)
		options_cancel();
	if (!s.screen_on)
		screen(1);
	/* вернуть карту — прокруткой вверх (или «Извлечь» на компьютере, или выдернуть кабель); отмены нет */
	slide_open(LV_SYMBOL_USB, "Карта у компьютера", tx("Чтобы вернуть её плееру, прокрутите колесо вверх"),
		   "вернуть карту", -1, NULL, usb_return, NULL);
	lv_refr_now(NULL);
}

/* «Стоковый плеер»: перезагрузка в сток на одну загрузку (обновление прошивки — там) */
static int quit_req, power_off_req;

bool xmb_power_off_requested(void)
{
	return power_off_req;
}

/* батарея, как в Sonix: на 5 % — одно предупреждение (снова — после 10 %); на 1 % три чтения подряд — отсчёт 10 с,
 * зарядка его отменяет, потом всё сохраняется и плеер выключается. Датчик бывает врёт нулём: при напряжении от
 * 3600 мВ не выключать. Работает и с погашенным экраном */
static void battery_guard(void)
{
	static uint32_t last, cd_end;
	static int warned, low;
	uint32_t now = lv_tick_get();
	if (now - last < (cd_end ? 1000u : 5000u))
		return;
	last = now;
	int chg, cap = platform_battery(&chg), mv = platform_battery_mv();
	if (cap < 0)
		return;
	if (chg || cap >= 10)
		warned = 0;
	if (!chg && cap <= 5 && cap > 1 && !warned) {
		warned = 1;
		if (!s.screen_on)
			screen(1);
		toast("Батарея %d%% — подключите зарядку", cap);
	}
	low = !chg && cap <= 1 && (mv < 0 || mv < 3600) ? low + 1 : 0;
	if (cd_end && chg) {
		cd_end = 0;
		toast("%s", "Зарядка — выключение отменено");
	} else if (!cd_end && low >= 3) {
		cd_end = now + 10000;
		if (!s.screen_on)
			screen(1);
	}
	if (!cd_end)
		return;
	int left = (int)((int32_t)(cd_end - now) + 999) / 1000;
	if (left > 0) {
		toast("Батарея села — выключение через %d с", left);
		return;
	}
	if (s.playing)
		toggle_play();
	save_flush();
	power_off_req = 1;
}

static void stock_do(void)
{
	save_flush();
	quit_req = 1;
}

bool xmb_quit_requested(void)
{
	return quit_req;
}

static void select_current(void)
{
	column_t *c = cur_col();
	if (s.at_cats) {
		s.at_cats = 0;
		return;
	}
	if (!c->count)
		return;
	const node_t *n = &c->nodes[c->sel];
	if (n->action == A_USB) {
		usb_menu();
	} else if (n->action == A_STOCK) {
		slide_open(LV_SYMBOL_REFRESH, "Перезагрузить в сток?",
			   tx("Один раз загрузится стоковый плеер HiBy, следующая загрузка — снова XMB"), "перезагрузить", 1,
			   "Отмена", stock_do, NULL);
	} else if (n->action == A_BTN_RESET) {
		memcpy(s.btn, btn_default, sizeof(s.btn));
		refresh_subs();
		toast("%s", "Кнопки — как по умолчанию");
	} else if (n->action == A_EQ) {
		eq_open();
	} else if (n->action == A_SEARCH) {
		kb_open(-1, "Поиск", search_q);
	} else if (n->action == A_BTDEV) {
		const bt_dev_t *d = &bt_list[n->track];
		if (d->connected) {
			bt_disconnect(d->mac);
		} else {
			bt_connect(d->mac);
			toast("Подключение к «%s»…", d->name);
		}
	} else if (n->action == A_BTSCAN) {
		bt_scan();
	} else if (n->action == A_WIFISCAN) {
		wifi_scan();
	} else if (n->action == A_PODUPDATE) {
		if (!wifi_online())
			toast("%s", "Включите Wi-Fi: Настройки → Wi-Fi");
		else if (!pod_feeds())
			toast("%s", "Положите ссылки на RSS в Podcasts/подписки.txt");
		else
			pod_update();
	} else if (n->action == A_WIFINET) {
		const wifi_net_t *w = &wifi_list[n->track];
		if (w->connected) {
			toast("%s", "Уже подключено — удержание колеса: забыть сеть");
		} else if (w->saved || !w->secured) {
			wifi_connect(w->ssid, w->saved ? NULL : "");
			toast("Подключение к «%s»…", w->ssid);
		} else {
			snprintf(wifi_kb_ssid, sizeof(wifi_kb_ssid), "%s", w->ssid);
			kb_open(KB_WIFI, "Пароль сети", "");
		}
	} else if (n->action == A_NEWPL) {
		kb_open(s.npl, "Новый плейлист", "");
	} else if (n->action == A_TRACK && n->track == s.track && !s.resume) {
		player_show(1);   /* играющий трек — открыть плеер, а не начать заново */
	} else if (n->action == A_TRACK) {
		play_queue(c->nodes, c->count, c->sel);
	} else if (n->opts) {
		options_open(n);
	} else if (n->kids && s.depth + 1 < MAX_DEPTH) {
		if (n->kids == about_items)
			about_fill();
		if (n->kids == bt_nodes && !s.bt_off)
			bt_scan();
		if (n->kids == wifi_nodes && !s.wifi_off)
			wifi_scan();   /* список — сразу с поиском: рядом могут быть новые наушники */
		column_t *k = &s.stack[s.depth + 1];
		int same = k->nodes == n->kids && k->count == n->nkids, sel = k->sel;   /* вернулись в тот же список */
		column_fill(k, n->kids, n->nkids);
		k->grid = n->grid && s.album_view == 0;
		k->big = n->grid && s.album_view == 2;
		if (n->kids == q_nodes && s.qi < n->nkids)   /* очередь открывается на играющем */
			k->sel = k->pos = s.qi;
		else if (same) {
			k->sel = k->pos = sel;
			k->rpos = k->grid ? sel / GRID_COLS : 0;
		}
		s.depth++;
		layout();
	}
}

static void about_fill(void)
{
	snprintf(about_sub[0], 64, tx("сборка %s"), __DATE__);
	if (s.lib)
		snprintf(about_sub[1], 64, tx("%d треков · %d альбомов · %d исполнителей"), s.lib->ntracks, s.lib->nalbums,
			 s.lib->nartists);
	struct statvfs v;
	const char *root = music_root();
	if (root && !statvfs(root, &v) && v.f_blocks)
		snprintf(about_sub[2], 64, tx("свободно %.1f ГБ из %.1f ГБ"), (double)v.f_bavail * v.f_frsize / 1e9,
			 (double)v.f_blocks * v.f_frsize / 1e9);
	else
		snprintf(about_sub[2], 64, "%s", tx("нет карты"));
	snprintf(about_sub[3], 64, s.bat_chg ? tx("%d%% · заряжается") : "%d%%", s.bat);
}

static void go_back(void)
{
	if (s.depth > 0)
		s.depth--;
}

/* ---------- эквалайзер ---------- */

static int eq_db(int band)
{
	return s.eq_preset == EQ_CUSTOM ? s.eq_custom[band] - 12 : eq_preset_db[s.eq_preset][band];
}

static void eq_apply(void)
{
	float g[EQ_BANDS];
	for (int i = 0; i < EQ_BANDS; i++)
		g[i] = (float)eq_db(i);
	audio_set_eq(s.eq_on, g);
	update_modes();
	if (s.eq_panel)
		lv_obj_invalidate(s.eq_panel);
}

#define EQ_X0 22
#define EQ_STEP 29
#define EQ_MID 236     /* 0 дБ */
#define EQ_PX 9        /* пикселей на 1 дБ: ±12 дБ — ±108 px */

static void eq_draw(lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer(e);
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	lv_draw_label_dsc_t tl;
	lv_draw_label_dsc_init(&tl);
	tl.font = s.f_bat;
	tl.align = LV_TEXT_ALIGN_CENTER;
	char buf[16];
	/* линия 0 дБ */
	d.bg_color = lv_color_white();
	d.bg_opa = 50;
	lv_area_t z = { EQ_X0 - 10, EQ_MID, EQ_X0 + EQ_STEP * (EQ_BANDS - 1) + 10, EQ_MID };
	lv_draw_rect(l, &d, &z);
	for (int i = 0; i < EQ_BANDS; i++) {
		int cx = EQ_X0 + i * EQ_STEP, v = eq_db(i), y = EQ_MID - v * EQ_PX, sel = i == s.eq_band;
		lv_color_t col = sel ? lv_color_hex(ACCENT) : lv_color_white();
		d.radius = 2;
		d.bg_color = lv_color_white();
		d.bg_opa = 30;
		lv_area_t tr = { cx - 2, EQ_MID - 12 * EQ_PX, cx + 2, EQ_MID + 12 * EQ_PX };
		lv_draw_rect(l, &d, &tr);
		d.bg_color = col;
		d.bg_opa = sel ? 255 : 170;
		lv_area_t fill = { cx - 4, v >= 0 ? y : EQ_MID, cx + 4, v >= 0 ? EQ_MID : y };
		lv_draw_rect(l, &d, &fill);
		d.radius = LV_RADIUS_CIRCLE;
		lv_area_t knob = { cx - (sel ? 8 : 6), y - (sel ? 8 : 6), cx + (sel ? 8 : 6), y + (sel ? 8 : 6) };
		lv_draw_rect(l, &d, &knob);
		tl.color = col;
		tl.opa = sel ? 255 : 160;
		tl.text = eq_labels[i];
		lv_area_t la = { cx - 16, EQ_MID + 12 * EQ_PX + 14, cx + 16, EQ_MID + 12 * EQ_PX + 30 };
		lv_draw_label(l, &tl, &la);
		if (sel) {
			snprintf(buf, sizeof(buf), "%+d", v);
			tl.text = buf;
			tl.text_local = 1;
			lv_area_t va = { cx - 16, EQ_MID - 12 * EQ_PX - 30, cx + 16, EQ_MID - 12 * EQ_PX - 14 };
			lv_draw_label(l, &tl, &va);
		}
	}
	/* подсказка */
	tl.color = lv_color_white();
	tl.opa = 140;
	tl.font = s.f_sub;
	tl.text = tx(s.eq_adjust ? "колесо — уровень, клик — готово" : "колесо — полоса, клик — изменить");
	lv_area_t ha = { 0, SCR_H - 84, SCR_W - 1, SCR_H - 66 };
	lv_draw_label(l, &tl, &ha);
}

/* редактор: свой пресет начинается с текущего */
static void eq_open(void)
{
	if (s.eq_preset != EQ_CUSTOM) {
		for (int i = 0; i < EQ_BANDS; i++)
			s.eq_custom[i] = eq_db(i) + 12;
		s.eq_preset = EQ_CUSTOM;
	}
	s.eq_on = 1;
	s.eq_edit = 1;
	s.eq_adjust = 0;
	set_hidden(s.eq_panel, false);
	eq_apply();
	refresh_subs();
}

static void eq_close(void)
{
	s.eq_edit = 0;
	set_hidden(s.eq_panel, true);
	refresh_subs();
}

static void eq_set(int band, int db)
{
	db = db < -12 ? -12 : db > 12 ? 12 : db;
	if (s.eq_custom[band] != db + 12) {
		s.eq_custom[band] = db + 12;
		eq_apply();
	}
}

/* палец по полосам: ближайшая полоса и уровень под пальцем */
static void eq_touch(lv_point_t p)
{
	int band = (p.x - EQ_X0 + EQ_STEP / 2) / EQ_STEP;
	band = band < 0 ? 0 : band >= EQ_BANDS ? EQ_BANDS - 1 : band;
	s.eq_band = band;
	eq_set(band, (int)lround((EQ_MID - p.y) / (double)EQ_PX));
}

/* спектр звука — для фонов, которые его показывают (у «Волны PSP» — только уровень), пока экран включён */
static void spectrum_update(void)
{
	audio_set_spectrum(s.screen_on && s.wave != BG_WAVE_PSP && s.wave != BG_WAVE_OFF);
}

static void screen(int on)
{
	s.screen_on = on;
	spectrum_update();
	set_hidden(s.off, on);
	s.input_ms = lv_tick_get();
}

int xmb_input_name(const char *name)
{
	static const struct { const char *name; xmb_input_t in; } map[] = {
		{ "up", XMB_WHEEL_UP }, { "down", XMB_WHEEL_DOWN }, { "enter", XMB_SELECT }, { "esc", XMB_BACK },
		{ "left", XMB_CAT_PREV }, { "right", XMB_CAT_NEXT }, { "play", XMB_PLAY }, { "next", XMB_NEXT_TRACK },
		{ "prev", XMB_PREV_TRACK }, { "nextalbum", XMB_NEXT_ALBUM }, { "fav", XMB_FAV }, { "menu", XMB_MENU },
		{ "addpl", XMB_ADD_PL }, { "power", XMB_POWER }, { "shade", XMB_SHADE },
	};
	if (s.kb_panel && !strncmp(name, "type:", 5)) {   /* набор в открытую клавиатуру */
		lv_textarea_set_text(s.kb_ta, name + 5);
		return 1;
	}
	if (!strcmp(name, "usb:on") || !strcmp(name, "usb:off")) {   /* кабель к компьютеру — для проверки экранов */
		xmb_usb_host(name[5] == 'n', 1);
		return 1;
	}
	if (!strcmp(name, "usb:storage")) {
		xmb_storage_prepare();
		return 1;
	}
	if (!strncmp(name, "btn:", 4) && name[4] && name[5]) {   /* кнопка на корпусе: btn:<кнопка><жест>, как в настройках */
		xmb_button(name[4] - '0', name[5] - '0');
		return 1;
	}
	if (!strcmp(name, "player")) {   /* в жизни — касанием мини-плеера */
		player_show(1);
		return 1;
	}
	if (!strcmp(name, "lyrics")) {
		toggle_mode(PM_LYRICS);
		return 1;
	}
	if (!strcmp(name, "prevalbum")) {   /* в жизни — кнопкой (BA_PREV_ALBUM) или свайпом */
		prev_group();
		return 1;
	}
	if (s.kb_panel && !strcmp(name, "kbok")) {
		if (s.kb_pl == -1)
			search_done();
		else
			kb_done();
		return 1;
	}
	for (unsigned i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(name, map[i].name)) {
			xmb_input(map[i].in);
			return 1;
		}
	return 0;
}

/* громкость колесом: в плеере и при погашенном экране */
static void volume_step(int dir)
{
	s.volume = volume_seg(s.volume, dir);
	audio_set_volume(s.volume);
	player_set_state(s.playing, s.pos, s.volume);
	player_show_volume();
}

void xmb_input(xmb_input_t in)
{
	s.input_ms = lv_tick_get();
	s.kin = 0;
	if (in == XMB_POWER || (in == XMB_SELECT && !s.screen_on)) {   /* экран будят кнопка питания и клик колеса */
		screen(!s.screen_on);
		return;
	}
	if (!s.screen_on && (in == XMB_WHEEL_UP || in == XMB_WHEEL_DOWN)) {   /* погашенный экран: колесо — громкость */
		volume_step(in == XMB_WHEEL_UP ? 1 : -1);
		return;
	}
	/* погашенный экран: работают кнопки воспроизведения, меню — нет */
	if (!s.screen_on && in != XMB_PLAY && in != XMB_NEXT_TRACK && in != XMB_PREV_TRACK && in != XMB_NEXT_ALBUM)
		return;
	if (slide_input(in))
		return;
	if (in == XMB_NEXT_ALBUM) {
		next_group();
		return;
	}
	if (s.kb_panel) {   /* клавиатура — пальцем, колесо двигает курсор по буквам, «назад» закрывает */
		if (in == XMB_WHEEL_UP)
			lv_textarea_cursor_left(s.kb_ta);
		else if (in == XMB_WHEEL_DOWN)
			lv_textarea_cursor_right(s.kb_ta);
		else if (in == XMB_BACK)
			kb_close();
		return;
	}
	if (in == XMB_MENU) {
		if (player_shown() && !s.opt_open)
			player_more();   /* в плеере долгое нажатие — меню «Ещё» */
		else if (!s.eq_edit && !s.sh_open)
			ctx_open();
		return;
	}
	if (in == XMB_FAV || in == XMB_ADD_PL) {
		if (in == XMB_FAV)
			toggle_fav();
		else
			playlist_menu();
		return;
	}
	if (in == XMB_PLAY) {
		toggle_play();
		return;
	}
	if (in == XMB_NEXT_TRACK) {
		next_track(1, 0);
		return;
	}
	if (in == XMB_PREV_TRACK) {
		prev_track();
		return;
	}
	if (in == XMB_SHADE) {
		shade_set(!s.sh_open);
		return;
	}

	if (s.eq_edit) {
		int d = in == XMB_WHEEL_DOWN ? 1 : in == XMB_WHEEL_UP ? -1 : 0;
		if (d && s.eq_adjust)
			eq_set(s.eq_band, eq_db(s.eq_band) - d);   /* по часовой — громче, как у громкости */
		else if (d)
			s.eq_band = s.eq_band + d < 0 ? 0 : s.eq_band + d >= EQ_BANDS ? EQ_BANDS - 1 : s.eq_band + d;
		else if (in == XMB_SELECT)
			s.eq_adjust = !s.eq_adjust;
		else if (in == XMB_BACK && s.eq_adjust)
			s.eq_adjust = 0;
		else if (in == XMB_BACK)
			eq_close();
		lv_obj_invalidate(s.eq_panel);
		return;
	}

	if (s.sh_open) {   /* шторка — пальцем; колесо — громкость, клик и «назад» закрывают */
		if (in == XMB_WHEEL_DOWN || in == XMB_WHEEL_UP) {
			s.volume = volume_seg(s.volume, in == XMB_WHEEL_UP ? -1 : 1);
			audio_set_volume(s.volume);
			player_set_state(s.playing, s.pos, s.volume);
			lv_obj_invalidate(s.sh_slider[1]);
		} else if (in == XMB_SELECT || in == XMB_BACK) {
			shade_set(0);
		}
		return;
	}

	if (s.opt_open) {
		if (in == XMB_WHEEL_DOWN && s.opt_sel < s.opt_count - 1)
			s.opt_sel++;
		else if (in == XMB_WHEEL_UP && s.opt_sel > 0)
			s.opt_sel--;
		else if (in == XMB_SELECT)
			options_apply();
		else if (in == XMB_BACK)
			options_cancel();
		if (s.opt_open)
			options_preview(s.opt_sel);
		return;
	}

	if (player_shown()) {
		if (in == XMB_WHEEL_UP || in == XMB_WHEEL_DOWN) {
			volume_step(in == XMB_WHEEL_UP ? 1 : -1);
		} else if (in == XMB_SELECT) {
			toggle_play();
		} else if (in == XMB_BACK) {
			player_show(0);
		}
		return;
	}

	column_t *c = cur_col();
	switch (in) {
	case XMB_WHEEL_DOWN:
	case XMB_WHEEL_UP: {
		int d = in == XMB_WHEEL_DOWN ? 1 : -1;
		if (s.at_cats) {
			set_cat(s.cat + d);
			break;
		}
		/* быстрое вращение: шаг растёт мягко (1…4) — без прыжков через весь список */
		uint32_t now = lv_tick_get(), gap = now - s.wheel_ms;
		s.wheel_ms = now;
		s.wheel_rate = gap > 250 ? 0 : s.wheel_rate * 0.6 + 0.4 * 1000.0 / (gap ? gap : 1);
		int step = s.wheel_rate < 16 ? 1 : s.wheel_rate < 28 ? 2 : s.wheel_rate < 40 ? 3 : 4;
		int to = c->sel + d * step;
		c->sel = to < 0 ? 0 : to >= c->count ? c->count - 1 : to;
		break;
	}
	case XMB_CAT_PREV:
	case XMB_CAT_NEXT:
		set_cat(s.cat + (in == XMB_CAT_NEXT ? 1 : -1));
		break;
	case XMB_SELECT:
		select_current();
		break;
	case XMB_BACK:
		go_back();
		break;
	default:
		break;
	}
}

static void prev_group(void);

/* жест кнопки на корпусе → действие из настроек; при погашенном экране работают все, кроме навигации по меню */
void xmb_button(int button, int gesture)
{
	if (button < 0 || button >= XMB_BTN_COUNT || gesture < 0 || gesture >= XMB_G_COUNT)
		return;
	s.input_ms = lv_tick_get();
	if (s.kb_panel)
		return;
	int act = s.btn[button][gesture];
	switch (act) {
	case BA_PLAY: xmb_input(XMB_PLAY); break;
	case BA_NEXT: xmb_input(XMB_NEXT_TRACK); break;
	case BA_PREV: xmb_input(XMB_PREV_TRACK); break;
	case BA_NEXT_ALBUM: xmb_input(XMB_NEXT_ALBUM); break;
	case BA_PREV_ALBUM: prev_group(); break;
	case BA_FWD:
	case BA_REW:
		if (s.track >= 0 && !s.resume) {
			double p = s.pos + (s.btn[button][gesture] == BA_FWD ? 10 : -10);
			p = p < 0 ? 0 : p > s.dur - 1 ? s.dur - 1 : p;
			seek_to(p);
			s.pos = p;
		}
		break;
	case BA_SHUFFLE: toggle_mode(PM_SHUFFLE); toast("%s", s.shuffle ? "Случайный порядок" : "По порядку"); break;
	case BA_REPEAT: toggle_mode(PM_REPEAT); toast("%s", repeat_names[s.repeat]); break;
	case BA_FAV: toggle_fav(); break;
	case BA_SCREEN: xmb_input(XMB_POWER); break;
	default: break;
	}
	/* значок — уже после действия: у паузы и избранного видно, каким стало состояние */
	const lv_font_t *f = &lv_font_montserrat_20;
	const char *g = act == BA_PLAY ? (s.playing ? LV_SYMBOL_PLAY : LV_SYMBOL_PAUSE) : act == BA_NEXT ? LV_SYMBOL_NEXT :
			act == BA_PREV ? LV_SYMBOL_PREV : act == BA_NEXT_ALBUM ? LV_SYMBOL_IMAGE "  " LV_SYMBOL_NEXT :
			act == BA_PREV_ALBUM ? LV_SYMBOL_PREV "  " LV_SYMBOL_IMAGE : act == BA_FWD ? "+10" :
			act == BA_REW ? "-10" : act == BA_SHUFFLE ? LV_SYMBOL_SHUFFLE : act == BA_REPEAT ? LV_SYMBOL_LOOP : NULL;
	if (act == BA_FAV && s.f_icons && s.track >= 0) {   /* сердечко — Font Awesome, как в плеере */
		int on = s.pls && pl_find(&s.pls[PL_FAVORITES], s.track) >= 0;
		g = on ? "\xEF\x80\x84" : "\xEF\x82\x8A";
		f = s.f_icons;
	}
	show_btn_hint(g, f);
}

bool xmb_button_has_double(int button)
{
	return button >= 0 && button < XMB_BTN_COUNT && s.btn[button][XMB_G_DOUBLE] != BA_NONE;
}

bool xmb_screen_on(void)
{
	return s.screen_on;
}

uint32_t xmb_sleep_after_ms(void)
{
	uint32_t d = (uint32_t)dev_sleep_sec[s.dev_sleep] * 1000u;
	/* мгновенное «не играет» (стык треков, перемотка) не повод засыпать с открытым потоком: тишина — всё время d;
	 * на внешнем питании не засыпаем вовсе (как в Compas) */
	if (!d || s.playing || s.sleep_end || s.kb_panel || s.bat_chg || usb.storage || pod_busy() ||
	    lv_tick_get() - s.active_ms < d)
		return 0;
	return d;
}

void xmb_sleep_end(void)
{
	wifi_park(0);
}

void xmb_wake(void)
{
	screen(1);
}

void xmb_screen_broken(void)
{
	toast("%s", "Экран сбоит — перезагрузите плеер");
}

static lv_obj_t *sleep_ov, *sleep_num, *sleep_hint;

void xmb_sleep_countdown(int left, const char *hint)
{
	if (left < 0) {
		if (sleep_ov)
			set_hidden(sleep_ov, true);
		return;
	}
	if (!sleep_ov) {
		sleep_ov = plain(lv_layer_top());
		lv_obj_set_size(sleep_ov, SCR_W, SCR_H);
		lv_obj_set_style_bg_color(sleep_ov, lv_color_black(), 0);
		lv_obj_set_style_bg_opa(sleep_ov, 220, 0);
		lv_obj_t *ic = label(sleep_ov, &lv_font_montserrat_40, LV_SYMBOL_POWER);
		lv_obj_align(ic, LV_ALIGN_CENTER, 0, -70);
		sleep_num = label(sleep_ov, s.f_title, "");
		lv_obj_set_width(sleep_num, SCR_W);
		lv_obj_set_style_text_align(sleep_num, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_align(sleep_num, LV_ALIGN_CENTER, 0, 0);
		sleep_hint = label(sleep_ov, s.f_sub, "");
		lv_obj_set_width(sleep_hint, SCR_W);
		lv_obj_set_style_text_align(sleep_hint, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_text_opa(sleep_hint, 170, 0);
		lv_obj_align(sleep_hint, LV_ALIGN_CENTER, 0, 34);
	}
	if (s.opt_open)
		options_cancel();   /* меню, открытое удержанием, уступает отсчёту */
	lv_label_set_text_fmt(sleep_num, tx("Сон через %d"), left);
	lv_label_set_text(sleep_hint, hint ? tx(hint) : "");
	set_hidden(sleep_ov, false);
}

void xmb_sleep_prepare(int radio_off)
{
	if (s.playing)
		toggle_play();
	if (radio_off)
		wifi_park(1);
	save_flush();
	xmb_sleep_countdown(-1, NULL);
	screen(0);
}

uint32_t xmb_last_input(void)
{
	return s.input_ms;
}

/* ---------- тач ---------- */

static column_t *drag_column(void)
{
	return s.at_cats ? NULL : cur_col();
}

/* тач в шторке: тап по плитке — переключить, ползунок — тянуть пальцем, свайп вверх или тап ниже — закрыть */
static void shade_touch(lv_event_code_t code, lv_point_t p, int dx, int dy)
{
	int top = (int)(-SHADE_H + SHADE_H * s.sh_t), py = p.y - top, sy = s.t_start.y - top;
	if (code == LV_EVENT_PRESSING) {
		for (int i = 0; i < 2 && !s.sh_drag; i++)
			if (abs(sy - SLIDER_Y(i)) < 22 && s.t_start.x > SLIDER_X - 16)
				s.sh_drag = 1 + i;
		if (s.sh_drag)
			shade_slider(s.sh_drag - 1, p.x);
		else if (dy < -10 && abs(dy) > abs(dx))
			s.sh_t = clamp01(1.0 + dy / (double)SHADE_H);
		return;
	}
	if (code != LV_EVENT_RELEASED)
		return;
	int tap = abs(dx) < 10 && abs(dy) < 10;
	if (s.sh_drag)
		s.sh_drag = 0;
	else if (dy < -60 || (tap && py > SHADE_H))
		shade_set(0);
	else if (tap)
		for (int i = 0; i < NTILE; i++)
			if (abs(p.x - TILE_X(i)) < 38 && py > TILE_Y(i) - TILE_R - 8 && py < TILE_Y(i) + TILE_R + 26)
				tile_tap(i);
}

static void touch_cb(lv_event_t *e)
{
	lv_event_code_t code = lv_event_get_code(e);
	lv_indev_t *indev = lv_indev_active();
	if (!s.screen_on || !indev)
		return;
	lv_point_t p;
	lv_indev_get_point(indev, &p);
	uint32_t now = lv_tick_get();
	int dx = p.x - s.t_start.x, dy = p.y - s.t_start.y;

	s.input_ms = now;
	if (code == LV_EVENT_PRESSED) {
		s.kin = 0;   /* касание останавливает бросок */
		s.t_nhist = 0;
		s.t_start = p;
		s.t_ms = now;
		s.t_mode = s.sh_open ? T_SHADE : T_TAP;
		/* «тач выключен» отключает только навигацию по меню: шторка и плеер доступны всегда */
		s.t_menu_off = s.touch_off && !s.sh_open && !player_shown();
		double f;
		if (!s.t_menu_off && !s.sh_open && !s.opt_open && !player_shown() && index_hit(p)) {
			column_t *c = index_column();
			s.idx_sel = c->sel;
			s.idx_pos = c->pos;
			s.idx_rpos = c->rpos;
			s.t_mode = T_INDEX;
			index_touch(p.y);
		} else if (!s.sh_open && !s.opt_open && s.track >= 0 && s.dur > 0 && player_bar_at(p.x, p.y, &f)) {
			s.t_mode = T_SEEK;
			s.seek_preview = f * s.dur;
			player_set_state(s.playing, s.seek_preview, s.volume);
		}
		return;
	}
	if (s.t_mode == T_SEEK) {   /* перемотка: тап или протяжка по полосе прогресса */
		if (code == LV_EVENT_PRESSING) {
			s.seek_preview = player_bar_frac(p.x) * s.dur;
			player_set_state(s.playing, s.seek_preview, s.volume);
		} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
			if (s.resume)
				s.resume_pos = s.seek_preview;
			else
				seek_to(s.seek_preview);
			s.pos = s.seek_preview;
			s.seek_preview = -1;
			s.t_mode = T_NONE;
		}
		return;
	}
	if (s.t_mode == T_INDEX && code == LV_EVENT_PRESSING && dx < -20 && abs(dx) > abs(dy) && s.depth > 0) {
		column_t *c = index_column();
		if (c) {
			c->sel = s.idx_sel;
			c->pos = s.idx_pos;
			c->rpos = s.idx_rpos;
			s.bubble_t = 0;
			set_hidden(s.bubble, true);
		}
		s.t_mode = T_BACKSWIPE;
		return;
	}
	if (s.t_mode == T_INDEX) {
		if (code == LV_EVENT_PRESSING)
			index_touch(p.y);
		else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
			s.t_mode = T_NONE;
		return;
	}
	if (code == LV_EVENT_LONG_PRESSED && s.t_mode == T_TAP && !s.t_menu_off && !s.eq_edit && !s.sh_open) {
		s.t_mode = T_NONE;   /* отпускание после долгого касания — не тап */
		ctx_open();
		return;
	}
	if (s.eq_edit) {
		if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) {
			if (p.y > EQ_MID - 12 * EQ_PX - 30 && p.y < EQ_MID + 12 * EQ_PX + 20) {
				s.t_mode = T_EQ;
				eq_touch(p);
				lv_obj_invalidate(s.eq_panel);
			}
		} else if (code == LV_EVENT_RELEASED) {
			int on_bars = p.y > EQ_MID - 12 * EQ_PX - 30 && p.y < EQ_MID + 12 * EQ_PX + 20;
			if (s.t_mode != T_EQ && abs(dx) < 10 && abs(dy) < 10 && on_bars) {
				eq_touch(p);   /* тап по полосе — уровень под пальцем */
				lv_obj_invalidate(s.eq_panel);
			} else if (s.t_mode != T_EQ && ((abs(dx) < 10 && abs(dy) < 10) || dx < -50)) {
				show_hint(0);   /* тап мимо полос или свайп влево — закрыть, со значком «назад» */
				eq_close();
			}
			s.t_mode = T_NONE;
		}
		return;
	}
	if (s.t_mode == T_SHADE) {
		shade_touch(code, p, dx, dy);
		if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST)
			s.t_mode = T_NONE;
		return;
	}
	if (code == LV_EVENT_PRESSING) {
		if (s.t_mode == T_TAP && (abs(dx) > 10 || abs(dy) > 10)) {
			int horiz = abs(dx) > abs(dy);
			int in_mini = s.t_start.y > SCR_H - MINI_H && !player_shown();
			if (s.t_menu_off && !(s.t_start.y < TOP_H + 16 && !horiz && dy > 0) && !(in_mini && dy < 0 && !horiz) &&
			    !(in_mini && horiz))
				s.t_mode = T_NONE;
			else if (s.t_start.y < TOP_H + 16 && !horiz && dy > 0 && !s.eq_edit)
				s.t_mode = T_PULL;   /* шторка — свайпом от верхней панели отовсюду, и из плеера */
			else if (s.opt_open && !horiz) {
				s.t_mode = T_VLIST;
				s.t_start_pos = s.opt_sel;
			} else if (s.opt_open)
				s.t_mode = T_BACKSWIPE;   /* боковое меню: свайп влево — назад */
			else if (player_shown())
				s.t_mode = T_PLAYER;
			else if (in_mini && horiz)
				s.t_mode = T_MINI;
			else if (s.t_start.y > SCR_H - MINI_H && !horiz && dy < 0)
				s.t_mode = T_SWIPE_UP;
			else if (horiz && !s.opt_open && s.depth > 0)
				s.t_mode = T_BACKSWIPE;
			else if (horiz && !s.opt_open && s.depth == 0) {
				s.t_mode = T_HCAT;
				s.t_start_pos = s.cat;
			} else if (!horiz && (s.opt_open || drag_column())) {
				s.t_mode = T_VLIST;
				s.t_start_pos = s.opt_open ? s.opt_sel : drag_column()->grid ? drag_column()->rpos : drag_column()->sel;
			} else
				s.t_mode = T_NONE;
		}
		if (s.t_mode == T_PLAYER && abs(dx) > abs(dy))
			player_drag(dx);
		if (s.t_mode == T_PULL) {
			s.sh_t = clamp01(dy / (double)SHADE_H);
		} else if (s.t_mode == T_HCAT && now - s.t_ms < 180 && abs(dx) > 24) {
			/* быстрый свайп: за пальцем не идём (тач 33 Гц даёт рывки), сразу анимация на одну категорию */
			set_cat(s.cat + (dx < 0 ? 1 : -1));
			s.t_mode = T_NONE;
		} else if (s.t_mode == T_HCAT) {
			/* «резинка» на один шаг: до половины шага ряд идёт за пальцем, дальше упирается */
			double off = fabs((double)dx) / CAT_STEP;
			int dir = dx < 0 ? 1 : -1;
			int edge = s.cat + dir < 0 || s.cat + dir >= NCAT;
			double v = off <= 0.5 ? off : 0.5 + (off - 0.5) * 0.3;
			s.cat_pos = s.t_start_pos + dir * fmin(v, 0.7) * (edge ? 0.4 : 1.0);
		} else if (s.t_mode == T_VLIST && !s.opt_open && drag_column()->grid) {
			column_t *c = drag_column();
			int rows = (c->count + GRID_COLS - 1) / GRID_COLS;
			double v = s.t_start_pos - (double)dy / grid_row_h();
			v = fmax(-0.35, fmin(rows - 1 + 0.35, v));
			int row = (int)lround(fmax(0, fmin(rows - 1, v)));
			c->rpos = v;
			c->sel = row * GRID_COLS + c->sel % GRID_COLS;
			if (c->sel >= c->count)
				c->sel = c->count - 1;
		} else if (s.t_mode == T_VLIST) {
			int n = s.opt_open ? s.opt_count : drag_column()->count;
			double step = s.opt_open ? OPT_ROW : drag_column()->big ? BIG_ROW : ROW;
			double v = s.t_start_pos - (double)dy / step;
			v = fmax(-0.35, fmin(n - 1 + 0.35, v));
			int sel = (int)lround(fmax(0, fmin(n - 1, v)));
			if (s.opt_open) {
				s.opt_pos = v;
				s.opt_sel = sel;
			} else {
				drag_column()->pos = v;
				drag_column()->sel = sel;
			}
		}
		s.t_hist[s.t_nhist % 8].t = now;
		s.t_hist[s.t_nhist % 8].y = p.y;
		s.t_nhist++;
	} else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
		int mode = s.t_mode;
		s.t_mode = T_NONE;
		if (code == LV_EVENT_PRESS_LOST)
			return;
		if (mode == T_TAP && s.t_menu_off) {
			if (p.y > SCR_H - MINI_H)
				player_show(1);
		} else if (mode == T_TAP && now - s.t_ms < 500) {
			int right = p.x >= SCR_W / 2;
			int m = player_mode_at(p.x, p.y), act = player_action_at(p.x, p.y);
			if (s.opt_open) {
				show_hint(right);
				xmb_input(right ? XMB_SELECT : XMB_BACK);
			} else if (act == PA_FAV) {
				toggle_fav();
			} else if (act == PA_ADD) {
				playlist_menu();
			} else if (act == PA_ALBUM) {
				show_hint(1);   /* отклик, как у тапа «выбрать»: плеер закрывается, альбом открыт на треке */
				reveal_album();
			} else if (m >= 0) {
				toggle_mode(m);
			} else if (player_shown()) {
				/* в плеере только его кнопки; закрыть — свайп вниз */
			} else if (p.y > SCR_H - MINI_H) {
				player_show(1);
			} else if (!s.at_cats && cur_col()->grid && grid_hit(cur_col(), p) >= 0) {
				/* сетка: тап выбирает обложку, тап по выбранной — открывает */
				column_t *c = cur_col();
				int hit = grid_hit(c, p);
				if (hit == c->sel)
					select_current();
				else
					c->sel = hit;
			} else {
				show_hint(right);
				xmb_input(right ? XMB_SELECT : XMB_BACK);
			}
		} else if (mode == T_PULL) {
			shade_set(dy > 60);
		} else if (mode == T_VLIST) {
			/* скорость пальца за последние ~100 мс, пикселей в секунду (вниз — плюс) */
			int last = (s.t_nhist - 1) % 8, k = last;
			for (int j = 1; j < 8 && j < s.t_nhist; j++) {
				int i = (s.t_nhist - 1 - j) % 8;
				k = i;
				if (now - s.t_hist[i].t >= 100)
					break;
			}
			double span = (s.t_hist[last].t - s.t_hist[k].t) / 1000.0;
			double v = span > 0.02 && now - s.t_hist[last].t < 80 ? (s.t_hist[last].y - s.t_hist[k].y) / span : 0;
			double step = s.opt_open ? OPT_ROW : drag_column()->grid ? grid_row_h() : drag_column()->big ? BIG_ROW : ROW;
			s.kin_v = -v / step;
			s.kin = fabs(s.kin_v) > 4 ? (s.opt_open ? 2 : 1) : 0;
		} else if (mode == T_HCAT && abs(dx) > CAT_STEP * 0.4) {
			set_cat(s.cat + (dx < 0 ? 1 : -1));
		} else if (mode == T_BACKSWIPE && (s.opt_open ? dx < -50 : dx > 50)) {   /* списки — вправо, меню справа — влево */
			show_hint(0);
			if (s.opt_open)
				options_cancel();
			else
				go_back();
		} else if (mode == T_MINI && abs(dx) > 50) {
			xmb_input(dx < 0 ? XMB_NEXT_TRACK : XMB_PREV_TRACK);   /* как карусель: влево — следующий */
			show_btn_hint(dx < 0 ? LV_SYMBOL_NEXT : LV_SYMBOL_PREV, &lv_font_montserrat_20);
		} else if (mode == T_SWIPE_UP && dy < -50) {
			player_show(1);
		} else if (mode == T_PLAYER) {
			int dir = dx < 0 ? 1 : -1;
			if (dy > 80 && abs(dy) > abs(dx)) {
				player_release(0);
				player_show(0);
			} else if (abs(dx) > 60 && switch_album(dir)) {
				player_release(dir);
			} else {
				player_release(0);
			}
		}
	}
}

/* ---------- init ---------- */

/* скругление углов: чёрная маска угла (A8, сглаженная) — под верхней панелью и внизу экрана, на системном слое
 * поверх всего. Не дуга окружности, а суперэллипс степени 4, как у Apple: кривизна нарастает плавно, без излома на стыке с
 * прямой. По глубине среза на диагонали — как окружность радиусом 12 */
#define CORNER_S 22     /* сторона кривой */
#define EDGE_TAPER 16   /* за кривой край рамки сходит на нет клином: длина — во столько раз больше ширины */
static uint8_t corner_px[4][(CORNER_S + EDGE_MAX) * (CORNER_S + EDGE_TAPER * EDGE_MAX)];
static lv_image_dsc_t corner_img[4];
static lv_obj_t *corner_obj[4];

/* нижние углы — у края экрана; при «Чёрном со скруглением» — на верхнем крае мини-плеера, как верхние у статус-бара
 * (пока не раскрыт плеер: он во весь экран) */
static void corners_place(void)
{
	int up = s.mini_style == MINI_FRAME && player_progress() <= 0;
	for (int c = 0; c < 4; c++)
		if (corner_obj[c])
			lv_obj_set_pos(corner_obj[c], c & 1 ? SCR_W - (int)corner_img[c].header.w : 0,
				       !(c & 2) ? TOP_H : (up ? SCR_H - MINI_H : SCR_H) - (int)corner_img[c].header.h);
}

/* чёрное ли в точке угла: across — от края экрана вглубь, along — от прямого края (панели) вдоль бока */
static int corner_black(double across, double along, int k, int len)
{
	if (along < CORNER_S) {
		double u = across - k;   /* от видимого края: рамка закрывает k пикселей */
		if (u < 0)
			return 1;
		double a = (CORNER_S - u) / CORNER_S, b = (CORNER_S - along) / CORNER_S;
		return u < CORNER_S && a * a * a * a + b * b * b * b > 1;
	}
	return len && across < k * (1.0 - (along - CORNER_S) / len);   /* клин вдоль края */
}

/* рамка корпуса закрывает несколько пикселей экрана по бокам (Настройки → Экран → Рамка): кривая угла касается
 * видимого края, а не края кадра, а дальше чёрное у края сходит на нет пологим клином — под углом обрыва не видно */
static void apply_edges(void)
{
	for (int c = 0; c < 4; c++) {   /* 0 — левый верхний, 1 — правый верхний, 2 — левый нижний, 3 — правый нижний */
		int k = c & 1 ? s.edge_r : s.edge_l, w = CORNER_S + k, len = EDGE_TAPER * k, h = CORNER_S + len;
		for (int y = 0; y < h; y++)
			for (int x = 0; x < w; x++) {
				int n = 0;   /* сглаживание: 4×4 точки на пиксель */
				for (int sy = 0; sy < 4; sy++)
					for (int sx = 0; sx < 4; sx++) {
						double fx = x + (sx + 0.5) / 4, fy = y + (sy + 0.5) / 4;
						n += corner_black(c & 1 ? w - fx : fx, c & 2 ? h - fy : fy, k, len);
					}
				corner_px[c][y * w + x] = (uint8_t)(n * 255 / 16);
			}
		corner_img[c] = (lv_image_dsc_t){ .header = { .magic = LV_IMAGE_HEADER_MAGIC, .cf = LV_COLOR_FORMAT_A8,
							      .w = w, .h = h, .stride = w },
						  .data_size = w * h, .data = corner_px[c] };
		if (!corner_obj[c]) {
			corner_obj[c] = lv_image_create(lv_layer_sys());
			lv_obj_set_style_image_recolor(corner_obj[c], lv_color_black(), 0);
			lv_obj_set_style_image_recolor_opa(corner_obj[c], LV_OPA_COVER, 0);
		}
		lv_image_set_src(corner_obj[c], &corner_img[c]);
		lv_obj_invalidate(corner_obj[c]);
	}
	corners_place();
}

/* ползунок шторки: сегменты, как громкость на PSP; при регулировке колесом — оранжевые */
static void slider_draw(lv_event_t *e)
{
	lv_obj_t *o = lv_event_get_target(e);
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	lv_layer_t *l = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(o, &a);
	int lit = idx == 0 ? (s.bright + 1) * SEG / 10 : (s.volume * SEG + 50) / 100;
	int adj = s.sh_drag == 1 + idx;
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.radius = 1;
	int w = lv_area_get_width(&a), sw = w / SEG;
	for (int i = 0; i < SEG; i++) {
		lv_area_t sa = { a.x1 + i * sw, a.y1, a.x1 + i * sw + sw - 3, a.y2 };
		d.bg_color = i < lit && adj ? lv_color_hex(ACCENT) : lv_color_white();
		d.bg_opa = i < lit ? 255 : 45;
		lv_draw_rect(l, &d, &sa);
	}
}

static void shade_create(lv_obj_t *parent)
{
	s.sh_panel = plain(parent);
	lv_obj_set_size(s.sh_panel, SCR_W, SHADE_H);
	lv_obj_set_style_radius(s.sh_panel, 16, 0);
	lv_obj_set_style_bg_opa(s.sh_panel, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s.sh_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_grad_color(s.sh_panel, lv_color_hex(0x1a1d24), 0);
	lv_obj_set_style_bg_grad_dir(s.sh_panel, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_opa(s.sh_panel, 235, 0);
	lv_obj_set_hidden(s.sh_panel, true);

	for (int i = 0; i < NTILE; i++) {
		s.sh_bg[i] = plain(s.sh_panel);
		lv_obj_set_size(s.sh_bg[i], TILE_R * 2, TILE_R * 2);
		lv_obj_set_pos(s.sh_bg[i], TILE_X(i) - TILE_R, TILE_Y(i) - TILE_R);
		lv_obj_set_style_radius(s.sh_bg[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(s.sh_bg[i], lv_color_black(), 0);   /* цвет — в layout_shade */
		s.sh_icon[i] = label(s.sh_bg[i], s.f_tile ? s.f_tile : &lv_font_montserrat_20,
				     s.f_tile ? tiles[i].fa : tiles[i].sym);
		lv_obj_center(s.sh_icon[i]);
		s.sh_label[i] = label(s.sh_panel, s.f_sub, tx(tiles[i].name));
		lv_obj_set_size(s.sh_label[i], 80, lv_font_get_line_height(s.f_sub));
		lv_obj_set_style_text_align(s.sh_label[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(s.sh_label[i], LV_LABEL_LONG_MODE_DOTS);
		lv_obj_set_pos(s.sh_label[i], TILE_X(i) - 40, TILE_Y(i) + TILE_R + 5);
	}

	const char *icons[2] = { LV_SYMBOL_EYE_OPEN, LV_SYMBOL_VOLUME_MAX };
	for (int i = 0; i < 2; i++) {
		lv_obj_t *ic = label(s.sh_panel, &lv_font_montserrat_20, icons[i]);
		lv_obj_set_size(ic, 32, 24);
		lv_obj_set_style_text_align(ic, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_pos(ic, 12, SLIDER_Y(i) - 12);
		s.sh_slider[i] = plain(s.sh_panel);
		lv_obj_set_size(s.sh_slider[i], SLIDER_W, 14);
		lv_obj_set_pos(s.sh_slider[i], SLIDER_X, SLIDER_Y(i) - 7);
		lv_obj_add_event_cb(s.sh_slider[i], slider_draw, LV_EVENT_DRAW_MAIN, (void *)(intptr_t)i);
	}
	lv_obj_t *grip = plain(s.sh_panel);   /* язычок: шторку тянут вверх */
	lv_obj_set_size(grip, 36, 4);
	lv_obj_set_pos(grip, (SCR_W - 36) / 2, SHADE_H - 12);
	lv_obj_set_style_radius(grip, 2, 0);
	lv_obj_set_style_bg_color(grip, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(grip, 90, 0);
}

/* смещения строк от центра пункта по высоте шрифтов меню */
/* оптический центр строки смешанного регистра — между серединой строчных (их в слове большинство) и серединой
 * заглавных (заглавная в начале тянет вверх); выносные (р, у) в счёт не идут. Возвращает: от верха строки до базовой
 * линии и высоту этого центра над базовой линией, умноженную на 2 */
static void font_xheight(const lv_font_t *f, int *asc, int *xh)
{
	lv_font_glyph_dsc_t g;
	*asc = f->line_height - f->base_line;
	int x = lv_font_get_glyph_dsc(f, &g, 'x', 0) && g.box_h > 0 ? g.box_h + g.ofs_y : f->line_height / 3;
	int cap = lv_font_get_glyph_dsc(f, &g, 'H', 0) && g.box_h > 0 ? g.box_h + g.ofs_y : x * 4 / 3;
	*xh = (x + cap) / 2;
}

static void size_metrics(void)
{
	int lt = lv_font_get_line_height(s.m_item), ls = lv_font_get_line_height(s.m_sub);
	int at, xt, as, xs;
	font_xheight(s.m_item, &at, &xt);
	font_xheight(s.m_sub, &as, &xs);
	s.h_title = lt + 1;
	s.h_sub = ls;
	s.dy_title = -(at - xt / 2);   /* одна строка: середина строчных — на центре значка */
	/* с подписью: блок от верха строчных названия до базовой линии подписи — по центру */
	s.dy_title_sel = -((at - xt) + (lt - 1 + as)) / 2;
	s.dy_sub = s.dy_title_sel + lt - 1;
}

static void apply_size(void)
{
	sz = &ui_sizes[s.ui_size];
	if (s.m_item != &lv_font_montserrat_14) {
		lv_tiny_ttf_set_size(s.m_title, sz->f_title);
		lv_tiny_ttf_set_size(s.m_item, sz->f_item);
		lv_tiny_ttf_set_size(s.m_sub, sz->f_sub);
	}
	size_metrics();
	for (int i = 0; i < NCAT; i++)
		column_widths(&s.cat_col[i]);
	for (int d = 1; d < MAX_DEPTH; d++)
		column_widths(&s.stack[d]);
	lv_obj_set_pos(s.cat_title, CAT_X - 70, CAT_Y + sz->cat_icon_sel / 2 + 6);
	lv_obj_report_style_change(NULL);   /* метки пересчитывают текст под новые размеры шрифтов */
	layout();
}

/* ---------- настройки: файл key=value на карте ---------- */

static const struct {
	const char *key;
	int *val, max;
} settings[] = {
	{ "theme", &s.theme, 11 }, { "bg", &s.wave, BG_WAVE_COUNT - 1 }, { "touch_off", &s.touch_off, 1 },
	{ "bright", &s.bright, 9 }, { "bt_off", &s.bt_off, 1 }, { "wifi_off", &s.wifi_off, 1 }, { "player_bg", &s.player_bg, 1 },
	{ "codec", &s.codec, 5 }, { "shuffle", &s.shuffle, 1 }, { "repeat", &s.repeat, 2 }, { "eq_on", &s.eq_on, 1 },
	{ "ui_size", &s.ui_size, 3 }, { "volume", &s.volume, 100 }, { "screen_off", &s.screen_off, 5 },
	{ "album_view", &s.album_view, 2 }, { "gain", &s.gain, 2 }, { "replaygain", &s.rg_mode, 2 }, { "xfade", &s.xfade, 3 }, { "lang", &s.lang, LANG_COUNT - 1 },
	{ "kb_start", &s.kb_start, 2 }, { "kb_langs", &s.kb_langs, 1 },
	{ "mini_style", &s.mini_style, MINI_FRAME }, { "edge_l", &s.edge_l, EDGE_MAX }, { "edge_r", &s.edge_r, EDGE_MAX },
	{ "btn_next_s", &s.btn[0][0], BA_COUNT - 1 }, { "btn_next_l", &s.btn[0][1], BA_COUNT - 1 },
	{ "btn_next_d", &s.btn[0][2], BA_COUNT - 1 }, { "btn_play_s", &s.btn[1][0], BA_COUNT - 1 },
	{ "btn_play_l", &s.btn[1][1], BA_COUNT - 1 }, { "btn_play_d", &s.btn[1][2], BA_COUNT - 1 },
	{ "btn_scr_s", &s.btn[2][0], BA_COUNT - 1 }, { "btn_scr_d", &s.btn[2][2], BA_COUNT - 1 }, { "dev_sleep", &s.dev_sleep, 2 }, { "eq_preset", &s.eq_preset, EQ_CUSTOM },
	{ "eq0", &s.eq_custom[0], 24 }, { "eq1", &s.eq_custom[1], 24 }, { "eq2", &s.eq_custom[2], 24 },
	{ "eq3", &s.eq_custom[3], 24 }, { "eq4", &s.eq_custom[4], 24 }, { "eq5", &s.eq_custom[5], 24 },
	{ "eq6", &s.eq_custom[6], 24 }, { "eq7", &s.eq_custom[7], 24 }, { "eq8", &s.eq_custom[8], 24 },
	{ "eq9", &s.eq_custom[9], 24 },
};

static unsigned settings_hash(void)
{
	unsigned h = 2166136261u;
	for (unsigned i = 0; i < N(settings); i++)
		h = (h ^ (unsigned)*settings[i].val) * 16777619u;
	return h;
}

static const char *music_root(void)
{
	const char *root = getenv("XMB_MUSIC");
	return root ? root : XMB_DEVICE ? "/data/mnt/sd_0" : NULL;
}

static void settings_load(void)
{
	const char *root = music_root();
	if (!root)
		return;
	snprintf(s.settings_path, sizeof(s.settings_path), "%s/.xmb/settings.txt", root);
	FILE *f = fopen(s.settings_path, "r");
	char line[128];
	while (f && fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq)
			continue;
		*eq = 0;
		if (!strcmp(line, "wave")) {   /* старый ключ: была ещё «Пыль (плавная)» под номером 7 — номера после неё на 1 больше */
			int v = atoi(eq + 1);
			v = v <= 6 ? v : v - 1;
			s.wave = v < 0 ? 0 : v > BG_WAVE_COUNT - 1 ? BG_WAVE_COUNT - 1 : v;
			continue;
		}
		for (unsigned i = 0; i < N(settings); i++)
			if (!strcmp(line, settings[i].key)) {
				int v = atoi(eq + 1);
				*settings[i].val = v < 0 ? 0 : v > settings[i].max ? settings[i].max : v;
			}
	}
	if (f)
		fclose(f);
	s.settings_sig = settings_hash();
}

/* сохранение через секунду после последнего изменения: громкость колесом не пишет файл на каждый шаг */
static void settings_poll(void)
{
	unsigned h = settings_hash();
	uint32_t now = lv_tick_get();
	if (h == s.settings_sig || !s.settings_path[0]) {
		s.settings_ms = now;
		return;
	}
	if (now - s.settings_ms < 1000)
		return;
	char dir[300];
	snprintf(dir, sizeof(dir), "%s", s.settings_path);
	*strrchr(dir, '/') = 0;
	mkdir(dir, 0755);
	char *buf = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&buf, &len);
	if (f) {
		for (unsigned i = 0; i < N(settings); i++)
			fprintf(f, "%s=%d\n", settings[i].key, *settings[i].val);
		fclose(f);
		fsave(s.settings_path, buf, len);
		free(buf);
	}
	s.settings_sig = h;
}

void xmb_init(const char *font_dir)
{
	memset(&s, 0, sizeof(s));
	fsave_init();
	s.f_title = load_font(font_dir, "NotoSans-SemiBold.ttf", 17);
	s.f_item = load_font(font_dir, "NotoSans-Regular.ttf", 15);
	s.f_sub = load_font(font_dir, "NotoSans-Regular.ttf", 12);
	s.f_clock = load_font(font_dir, "NotoSans-SemiBold.ttf", 15);
	s.f_bat = load_font(font_dir, "NotoSans-SemiBold.ttf", BAT_FONT_PX);
	s.m_title = load_font(font_dir, "NotoSans-SemiBold.ttf", 17);
	s.m_item = load_font(font_dir, "NotoSans-Regular.ttf", 15);
	s.m_sub = load_font(font_dir, "NotoSans-Regular.ttf", 12);
	s.f_letter = load_font(font_dir, "NotoSans-SemiBold.ttf", 40);
	s.f_icons = load_font(font_dir, "FontAwesome4.ttf", 20);
	s.f_tile = load_font(font_dir, "FontAwesome4.ttf", 22);
	if (!s.f_title || !s.f_item || !s.f_sub || !s.f_clock || !s.f_bat || !s.m_title || !s.m_item || !s.m_sub || !s.f_letter)
		s.f_title = s.f_item = s.f_sub = s.f_clock = s.f_bat = s.m_title = s.m_item = s.m_sub = s.f_letter =
			(lv_font_t *)&lv_font_montserrat_14;
	else
		s.f_title->fallback = s.f_item->fallback = s.f_sub->fallback = s.f_clock->fallback = s.m_title->fallback =
			s.m_item->fallback = s.m_sub->fallback = &lv_font_montserrat_14;

	time_t now = time(NULL);
	s.theme = localtime(&now)->tm_mon;
	s.bright = 9;
	s.sleep = SLEEP_OFF;
	s.screen_off = 1;
	memcpy(s.btn, btn_default, sizeof(s.btn));
	for (int i = 0; i < EQ_BANDS; i++)
		s.eq_custom[i] = 12;
	s.volume = 30;
	s.album_view = 2;   /* «Список с обложками»: под колесо вертикальный список удобнее сетки */
	s.track = -1;
	s.ui_size = 1;
	s.seek_preview = -1;
	settings_load();
	bt_init(!s.bt_off);
	wifi_init(!s.wifi_off);
	pod_init(music_root());
	i18n_set(s.lang);
	sz = &ui_sizes[s.ui_size];
	size_metrics();
	s.cat = 1;
	s.cat_pos = 1;
	s.screen_on = 1;
	if (getenv("XMB_WAVE"))
		s.wave = atoi(getenv("XMB_WAVE")) % BG_WAVE_COUNT;

	lv_obj_t *scr = lv_screen_active();
	lv_obj_set_scrollable(scr, false);
	bg_create(scr);
	apply_theme();
	bg_prerender(theme_colors, N(theme_colors));
	apply_wave();
	apply_bright();
	s.main = plain(scr);
	lv_obj_set_size(s.main, SCR_W, SCR_H);
	s.glow = lv_image_create(s.main);
	lv_image_set_src(s.glow, bg_glow(GLOW));
	lv_obj_set_hidden(s.glow, true);

	for (int c = 0; c < NCAT; c++) {
		lv_obj_t *l = label(s.main, &lv_font_montserrat_24, cats[c].icon);
		lv_obj_set_size(l, 56, 44);
		lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_style_transform_pivot_x(l, 28, 0);
		lv_obj_set_style_transform_pivot_y(l, 22, 0);
		s.cat_icons[c] = l;
	}
	s.cat_title = label(s.main, s.m_title, "");
	lv_obj_set_width(s.cat_title, 140);
	lv_obj_set_style_text_align(s.cat_title, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(s.cat_title, CAT_X - 70, CAT_Y + sz->cat_icon_sel / 2 + 6);

	for (int i = 0; i < NCAT; i++) {
		s.cat_col[i].cont = plain(s.main);
		lv_obj_set_size(s.cat_col[i].cont, SCR_W, SCR_H);
	}
	for (int d = 1; d < MAX_DEPTH; d++) {
		s.stack[d].cont = plain(s.main);
		lv_obj_set_size(s.stack[d].cont, SCR_W, SCR_H);
		/* строки уровней — заранее: создание пула (64 объекта) в кадре первого входа давало рывок; пустая колонка
		 * скрыта целиком (column_cont_vis), обход дерева их не задевает */
		column_pool(&s.stack[d]);
		for (int i = 0; i < POOL; i++)
			hide_row(&s.stack[d].rows[i]);
		lv_obj_set_hidden(s.stack[d].cont, true);
	}

	/* мини-плеер */
	lv_obj_t *np = s.mini = plain(s.main);
	lv_obj_set_size(np, SCR_W, MINI_H);
	lv_obj_set_pos(np, 0, SCR_H - MINI_H);
	lv_obj_set_style_bg_color(np, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(np, LV_OPA_30, 0);
	lv_obj_t *track = s.mini_track = plain(np);
	lv_obj_set_size(track, SCR_W, 2);
	lv_obj_set_style_bg_color(track, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(track, 30, 0);
	s.mini_bar = plain(np);
	lv_obj_set_size(s.mini_bar, 0, 2);
	lv_obj_set_style_bg_color(s.mini_bar, lv_color_hex(ACCENT), 0);
	lv_obj_set_style_bg_opa(s.mini_bar, LV_OPA_COVER, 0);
	s.mini_thumb = plain(np);
	lv_obj_set_size(s.mini_thumb, 36, 36);
	lv_obj_set_pos(s.mini_thumb, 10, 9);
	lv_obj_set_style_bg_opa(s.mini_thumb, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(s.mini_thumb, lv_color_hex(EMPTY_BG), 0);
	lv_obj_t *note = label(s.mini_thumb, &lv_font_montserrat_16, LV_SYMBOL_AUDIO);
	lv_obj_set_style_text_color(note, lv_color_hex(EMPTY_FG), 0);
	lv_obj_center(note);
	s.mini_img = lv_image_create(s.mini_thumb);
	lv_obj_set_size(s.mini_img, 36, 36);
	lv_obj_set_hidden(s.mini_img, true);
	s.mini_text = label(np, s.f_item, "");
	lv_obj_set_pos(s.mini_text, 56, 8);
	lv_obj_set_size(s.mini_text, SCR_W - 56 - 58, 20);
	lv_label_set_long_mode(s.mini_text, LV_LABEL_LONG_MODE_DOTS);
	s.mini_artist = label(np, s.f_sub, "");
	lv_obj_set_style_text_opa(s.mini_artist, 160, 0);
	lv_obj_set_pos(s.mini_artist, 56, 28);
	lv_obj_set_size(s.mini_artist, SCR_W - 56 - 58, 17);
	lv_label_set_long_mode(s.mini_artist, LV_LABEL_LONG_MODE_DOTS);
	s.mini_time = label(np, s.f_sub, "");
	lv_obj_set_style_text_opa(s.mini_time, 170, 0);
	lv_obj_set_width(s.mini_time, 44);
	lv_obj_set_style_text_align(s.mini_time, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(s.mini_time, SCR_W - 54, 9);
	s.mini_icon = label(np, &lv_font_montserrat_14, "");
	lv_obj_set_width(s.mini_icon, 44);
	lv_obj_set_style_text_align(s.mini_icon, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(s.mini_icon, SCR_W - 54, 28);

	player_create(scr, s.f_title, s.f_item, s.f_sub);
	if (s.f_icons)
		player_set_icon_font(s.f_icons);

	/* панель вариантов: поверх плеера (там же список режимов) */
	s.opt_panel = plain(scr);
	lv_obj_set_size(s.opt_panel, OPT_W, SCR_H);
	lv_obj_set_style_bg_color(s.opt_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.opt_panel, 220, 0);
	lv_obj_set_pos(s.opt_panel, SCR_W, 0);
	lv_obj_set_hidden(s.opt_panel, true);

	s.idx_strip = plain(s.main);
	lv_obj_set_size(s.idx_strip, IDX_W, SCR_H);
	lv_obj_set_pos(s.idx_strip, SCR_W - IDX_W, 0);
	lv_obj_add_event_cb(s.idx_strip, index_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_set_hidden(s.idx_strip, true);

	s.eq_panel = plain(scr);
	lv_obj_set_size(s.eq_panel, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(s.eq_panel, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.eq_panel, 240, 0);
	lv_obj_add_event_cb(s.eq_panel, eq_draw, LV_EVENT_DRAW_MAIN, NULL);
	s.eq_title = label(s.eq_panel, s.f_title, tx("Эквалайзер"));
	lv_obj_set_pos(s.eq_title, 20, 44);
	lv_obj_set_hidden(s.eq_panel, true);
	s.bubble = plain(scr);
	lv_obj_set_size(s.bubble, 84, 84);
	lv_obj_set_pos(s.bubble, (SCR_W - 84) / 2, SCR_H / 2 - 62);
	lv_obj_set_style_radius(s.bubble, 16, 0);
	lv_obj_set_style_bg_color(s.bubble, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.bubble, 170, 0);
	s.bubble_lbl = label(s.bubble, s.f_letter, "");
	lv_obj_set_style_text_color(s.bubble_lbl, lv_color_hex(ACCENT), 0);
	lv_obj_center(s.bubble_lbl);
	lv_obj_set_hidden(s.bubble, true);

	/* слой тача */
	lv_obj_t *touch = plain(scr);
	lv_obj_set_size(touch, SCR_W, SCR_H);
	lv_obj_set_clickable(touch, true);
	lv_obj_add_event_cb(touch, touch_cb, LV_EVENT_ALL, NULL);

	/* верхний слой: шторка под верхней панелью */
	lv_obj_t *top = lv_layer_top();
	shade_create(top);
	lv_obj_t *tb = s.tb = plain(top);   /* всегда чёрная: под ней ничего не просвечивает и не темнеет дважды */
	lv_obj_set_size(tb, SCR_W, TOP_H);
	lv_obj_set_style_bg_color(tb, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(tb, LV_OPA_COVER, 0);
	s.tb_left = label(tb, s.f_clock, "");
	lv_obj_set_pos(s.tb_left, 12, 5);
	s.tb_right = label(tb, &lv_font_montserrat_14, "");
	lv_obj_set_width(s.tb_right, 160);
	lv_obj_set_style_text_align(s.tb_right, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(s.tb_right, SCR_W - 210, 7);
	s.tb_bat = plain(tb);
	lv_obj_set_size(s.tb_bat, 34, 16);
	lv_obj_set_pos(s.tb_bat, SCR_W - 44, 6);
	lv_obj_add_event_cb(s.tb_bat, battery_draw, LV_EVENT_DRAW_MAIN, NULL);
	s.tb_act = plain(tb);   /* между часами и батарейкой */
	lv_obj_set_size(s.tb_act, 200, 24);
	lv_obj_set_pos(s.tb_act, (SCR_W - 200) / 2, 2);
	lv_obj_add_event_cb(s.tb_act, activity_draw, LV_EVENT_DRAW_MAIN, NULL);
	lv_obj_set_hidden(s.tb_act, true);

	/* подсказка кнопки на корпусе: значок действия сверху по центру */
	s.bhint = plain(top);
	lv_obj_set_size(s.bhint, 44, 44);
	lv_obj_set_style_radius(s.bhint, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_bg_color(s.bhint, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.bhint, 120, 0);
	s.bhint_lbl = label(s.bhint, &lv_font_montserrat_20, "");
	lv_obj_set_hidden(s.bhint, true);

	/* короткие уведомления — в том же стиле, шире, со средним текстом; поверх плеера, шторки и клавиатуры */
	s.toast = plain(top);
	lv_obj_set_size(s.toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_style_min_height(s.toast, 44, 0);
	lv_obj_set_style_pad_hor(s.toast, 18, 0);
	lv_obj_set_style_pad_ver(s.toast, 11, 0);
	lv_obj_set_style_radius(s.toast, 22, 0);
	lv_obj_set_style_bg_color(s.toast, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.toast, 150, 0);
	s.toast_lbl = label(s.toast, s.f_item, "");
	lv_label_set_long_mode(s.toast_lbl, LV_LABEL_LONG_MODE_WRAP);
	lv_obj_set_style_text_align(s.toast_lbl, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_hidden(s.toast, true);

	/* подсказки тапа: назад / выбрать */
	const char *hint_icons[2] = { LV_SYMBOL_LEFT, LV_SYMBOL_OK };
	for (int i = 0; i < 2; i++) {
		s.hint[i] = plain(top);
		lv_obj_set_size(s.hint[i], 44, 44);
		lv_obj_set_style_radius(s.hint[i], LV_RADIUS_CIRCLE, 0);
		lv_obj_set_style_bg_color(s.hint[i], lv_color_black(), 0);
		lv_obj_set_style_bg_opa(s.hint[i], 120, 0);
		lv_obj_t *ic = label(s.hint[i], &lv_font_montserrat_20, hint_icons[i]);
		lv_obj_center(ic);
		lv_obj_set_hidden(s.hint[i], true);
	}

	s.off = plain(top);
	lv_obj_set_size(s.off, SCR_W, SCR_H);
	lv_obj_set_style_bg_color(s.off, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(s.off, LV_OPA_COVER, 0);
	lv_obj_set_hidden(s.off, true);

	s.next_qi = -1;
	audio_init();
	audio_set_gain(s.gain);
	audio_set_volume(s.volume);
	apply_xfade();
	eq_apply();
	library_start();
	for (int c = 0; c < NCAT; c++)
		column_fill(&s.cat_col[c], cats[c].items, cats[c].count);
	nav_restore(1);
	lv_label_set_text(s.cat_title, tx(cats[s.cat].title));
	apply_size();
	update_np_labels();
	update_modes();
	s.last_ms = s.input_ms = s.active_ms = lv_tick_get();
	layout();
	update_topbar();
	apply_edges();
	apply_mini();
	lv_timer_create(frame_cb, 16, NULL);
}
