/*
 * Полноэкранный плеер в духе Walkman/PSP: фон XMB с визуализатором остаётся видимым, без кнопок-коробок.
 * Обложка альбома, а без неё — анимированная кассета (как в плеере Sony Ericsson): катушки
 * крутятся при воспроизведении, плёнка перематывается слева направо по ходу трека.
 * Колесо — громкость (сегментированная полоска, как на PSP).
 */
#include "xmb_player.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cover.h"

#define W 320
#define H 480
#define ACCENT 0xff7a1a       /* оранжевый Walkman */
#define COVER 224
#define COVER_X ((W - COVER) / 2)
#define COVER_Y 40
#define CARD 170              /* соседние альбомы выглядывают по краям на CARD_PEEK */
#define CARD_PEEK 32
#define CARD_Y (COVER_Y + (COVER - CARD) / 2)
#define PITCH (W - CARD_PEEK + CARD / 2 - W / 2)   /* от центра обложки до центра соседней карточки */
#define CAS_W 208
#define CAS_H 132
#define CAS_X ((W - CAS_W) / 2)
#define CAS_Y (COVER_Y + (COVER - CAS_H - 28) / 2)
#define BAR_X 24
#define BAR_W 272
#define BAR_Y 362
#define MODES_Y 418
#define MODE_STEP (W / PM_COUNT)
#define FA_MORE "\xEF\x85\x81"   /* U+F141, многоточие — меню «Ещё» */
#define FA_LYRICS "\xEF\x80\xB6"   /* U+F036, строки текста */
#define NLYR 11               /* строк текста на экране (переиспользуются при прокрутке) */
#define LYR_Y (COVER_Y - 8)   /* текст — на месте обложки, чуть выше и ниже неё */
#define LYR_H (COVER + 14)
#define ACT_Y (BAR_Y - 24)        /* сердечко и «+» над шкалой времени, по краям */
#define ACT_X 38
#define VOL_SEG 30
#define MINI_H 52             /* высота мини-плеера: с него начинается раскрытие */
#define MINI_OPA 77           /* его затемнение (чёрный 30%) */
#define FULL_OPA PLAYER_DIM

static struct {
	lv_obj_t *root, *content, *cards, *cassette, *cover, *pending, *heart, *add, *title, *artist, *album, *format, *bar, *t_pos, *t_dur;
	lv_obj_t *mode_icon[PM_COUNT], *mode_dot[PM_COUNT], *mode_label[PM_COUNT];
	lv_obj_t *vol, *vol_bar, *vol_text;
	const track_t *track;
	int shown, playing, volume;
	double t, vol_t, pos, angle, acc;   /* t — раскрытие 0 (мини-плеер) … 1 (весь экран) */
	double off;                          /* сдвиг карусели, px */
	int dragging;
	const char *prev_album, *next_album, *prev_cover, *next_cover, *cover_path;
	const lv_image_dsc_t *cover_full;    /* обложка в полном размере; на экране — она или её уменьшенная копия */
	unsigned cover_gen;
	int fav;
	lv_obj_t *ab[2];
	/* текст песни: строки по слотам (слот i — строка с номером ≡ i по модулю NLYR), прокрутка — плавная */
	lv_obj_t *lyr, *lyr_line[NLYR];
	const lyrics_t *lyr_src;
	int lyr_on, lyr_idx[NLYR], lyr_cur, *lyr_y, lyr_total;
	double lyr_k, lyr_scroll;
	const lv_font_t *lyr_font;
} p;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h)
{
	lv_obj_t *o = lv_obj_create(parent);
	lv_obj_remove_style_all(o);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);
	lv_obj_set_pos(o, x, y);
	lv_obj_set_size(o, w, h);
	return o;
}

static lv_obj_t *text(lv_obj_t *parent, const lv_font_t *f, int y, int opa)
{
	lv_obj_t *l = lv_label_create(parent);
	lv_obj_set_style_text_font(l, f, 0);
	lv_obj_set_style_text_color(l, lv_color_white(), 0);
	lv_obj_set_style_text_opa(l, opa, 0);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_size(l, W - 32, lv_font_get_line_height(f) + 2);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_pos(l, 16, y);
	lv_label_set_text(l, "");
	return l;
}

static void fmt_time(char *buf, int s)
{
	sprintf(buf, "%d:%02d", s / 60, s % 60);
}

/* ---------- кассета ---------- */

/* кассета рисуется сама: прозрачность раскрытия плеера на неё не действует — множитель вручную */
static double draw_k = 1;

static void rect(lv_layer_t *l, int x1, int y1, int x2, int y2, uint32_t c, int opa, int r)
{
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = lv_color_hex(c);
	d.bg_opa = (lv_opa_t)(opa * draw_k);
	d.radius = r;
	lv_area_t a = { x1, y1, x2, y2 };
	lv_draw_rect(l, &d, &a);
}

static void circle(lv_layer_t *l, int cx, int cy, int r, uint32_t c, int opa)
{
	rect(l, cx - r, cy - r, cx + r, cy + r, c, opa, LV_RADIUS_CIRCLE);
}

static void reel(lv_layer_t *l, int cx, int cy, double tape, double angle)
{
	circle(l, cx, cy, (int)(12 + 14 * tape), 0x3a2618, 255);   /* намотанная плёнка */
	circle(l, cx, cy, 11, 0xe8e6e0, 255);                     /* катушка */
	circle(l, cx, cy, 4, 0x2a2d33, 255);
	for (int i = 0; i < 6; i++) {                               /* зубцы катушки */
		double a = angle + i * M_PI / 3;
		lv_draw_line_dsc_t d;
		lv_draw_line_dsc_init(&d);
		d.color = lv_color_hex(0x2a2d33);
		d.opa = (lv_opa_t)(255 * draw_k);
		d.width = 2;
		d.p1.x = cx + cos(a) * 5;
		d.p1.y = cy + sin(a) * 5;
		d.p2.x = cx + cos(a) * 10;
		d.p2.y = cy + sin(a) * 10;
		lv_draw_line(l, &d);
	}
}

static void cassette_draw(lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(p.cassette, &a);
	int x = a.x1, y = a.y1;
	double prog = p.track && p.track->duration && p.pos > 0 ? p.pos / p.track->duration : 0;
	draw_k = p.t >= 1 ? 1.0 : p.t * p.t;   /* как у остального содержимого */

	rect(l, x, y, x + CAS_W - 1, y + CAS_H - 1, 0x22252b, 255, 10);            /* корпус */
	rect(l, x + 2, y + 2, x + CAS_W - 3, y + CAS_H / 2, 0xffffff, 14, 9);       /* блик сверху */
	rect(l, x + 12, y + 10, x + CAS_W - 13, y + 84, 0xe6dfcf, 255, 6);          /* наклейка */
	rect(l, x + 12, y + 18, x + CAS_W - 13, y + 23, ACCENT, 255, 0);            /* полоса Walkman */
	rect(l, x + 12, y + 27, x + CAS_W - 13, y + 29, ACCENT, 160, 0);
	rect(l, x + 44, y + 40, x + CAS_W - 45, y + 76, 0x15171b, 255, 16);         /* окошко */
	reel(l, x + 72, y + 58, 1.0 - prog, p.angle);
	reel(l, x + CAS_W - 73, y + 58, prog, p.angle);
	rect(l, x + 50, y + CAS_H - 30, x + CAS_W - 51, y + CAS_H - 6, 0x2e3239, 255, 4);   /* низ с окнами головки */
	for (int i = 0; i < 4; i++)
		circle(l, x + 70 + i * 23, y + CAS_H - 18, 3, 0x15171b, 255);
	circle(l, x + 10, y + CAS_H - 10, 3, 0x15171b, 255);                        /* винты */
	circle(l, x + CAS_W - 11, y + CAS_H - 10, 3, 0x15171b, 255);

	/* отражение */
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_grad.dir = LV_GRAD_DIR_VER;
	d.bg_grad.stops[0].color = lv_color_white();
	d.bg_grad.stops[0].opa = (lv_opa_t)(40 * draw_k);
	d.bg_grad.stops[0].frac = 0;
	d.bg_grad.stops[1].color = lv_color_white();
	d.bg_grad.stops[1].opa = 0;
	d.bg_grad.stops[1].frac = 255;
	d.bg_grad.stops_count = 2;
	d.radius = 6;
	lv_area_t r = { x + 6, y + CAS_H + 4, x + CAS_W - 7, y + CAS_H + 26 };
	lv_draw_rect(l, &d, &r);
}

/* уменьшенная копия обложки: ближайший пиксель по таблице. Масштаб при отрисовке считает преобразование на каждый
 * пиксель и в движении карусели не укладывался в кадр. Слот 0 — текущая обложка, 1 и 2 — соседи */
typedef struct {
	const void *data;
	int size;
	unsigned gen;
	lv_image_dsc_t dsc;
	uint16_t px[COVER * COVER];
} scaled_t;
static scaled_t scaled_buf[3];

static const lv_image_dsc_t *scaled(int slot, const lv_image_dsc_t *src, int size)
{
	int n = (int)src->header.w;
	if (size >= n)
		return src;
	unsigned gen = cover_generation();   /* место в кэше обложек могло достаться другой картинке */
	scaled_t *b = &scaled_buf[slot];
	if (b->data == src->data && b->size == size && b->gen == gen)
		return &b->dsc;
	int xi[COVER];
	for (int x = 0; x < size; x++)
		xi[x] = (x * n + n / 2) / size;
	const uint16_t *in = (const uint16_t *)src->data;
	for (int y = 0; y < size; y++) {
		const uint16_t *row = in + (y * n + n / 2) / size * n;
		uint16_t *o = b->px + y * size;
		for (int x = 0; x < size; x++)
			o[x] = row[xi[x]];
	}
	lv_image_dsc_t *d = &b->dsc;
	memset(d, 0, sizeof(*d));
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_RGB565;
	d->header.w = size;
	d->header.h = size;
	d->header.stride = size * 2;
	d->data_size = size * size * 2;
	d->data = (const uint8_t *)b->px;
	b->data = src->data;
	b->size = size;
	b->gen = gen;
	return d;
}

/* соседние альбомы: квадратные карточки по краям, без картинки — цвет альбома и нота */
/* карусель непрерывна: обложка и соседи — один ряд с шагом PITCH, размер плавно меняется от COVER в центре
 * до CARD у края; при перелистывании новая обложка начинает движение с места соседней карточки */
static double slot_k(double cx)   /* 0 — в центре, 1 — на месте соседа */
{
	double k = fabs(cx - W / 2.0) / PITCH;
	return k > 1 ? 1 : k;
}

static void card(lv_layer_t *l, int slot, double cx, int top, const char *album, const char *cover)
{
	double k = slot_k(cx);
	/* соседи рисуются здесь сами: прозрачность раскрытия плеера (у содержимого — t²) и текста песни — вручную */
	double fade = (p.t >= 1 ? 1.0 : p.t * p.t) * (1.0 - p.lyr_k);
	int sz = (int)(COVER - (COVER - CARD) * k + 0.5), opa = (int)((255 - 105 * k) * fade);
	int x = (int)(cx - sz / 2.0), y = top + (COVER - sz) / 2;
	/* в покое — миниатюра своего размера (сглаженная); в движении и пока её нет — уменьшенная большая,
	 * а не заглушка (иначе на кадр мелькает цветной квадрат) */
	const lv_image_dsc_t *small = p.off == 0 ? cover_get(cover, CARD) : NULL;
	const lv_image_dsc_t *img = small ? small : cover_get(cover, COVER);
	if (img) {
		lv_draw_image_dsc_t id;
		lv_draw_image_dsc_init(&id);
		id.src = small ? small : scaled(slot, img, sz);
		id.opa = (lv_opa_t)opa;
		lv_area_t a = { x, y, x + sz - 1, y + sz - 1 };
		lv_draw_image(l, &id, &a);
		return;
	}
	(void)album;
	lv_draw_rect_dsc_t d;
	lv_draw_rect_dsc_init(&d);
	d.bg_color = lv_color_hex(EMPTY_BG);
	d.bg_opa = (lv_opa_t)opa;
	lv_area_t a = { x, y, x + sz - 1, y + sz - 1 };
	lv_draw_rect(l, &d, &a);
	lv_draw_label_dsc_t tl;
	lv_draw_label_dsc_init(&tl);
	tl.font = &lv_font_montserrat_32;
	tl.color = lv_color_hex(EMPTY_FG);
	tl.opa = (lv_opa_t)opa;
	tl.align = LV_TEXT_ALIGN_CENTER;
	tl.text = LV_SYMBOL_AUDIO;
	lv_area_t ta = { x, y + sz / 2 - 18, x + sz - 1, y + sz / 2 + 18 };
	lv_draw_label(l, &tl, &ta);
}

static void cards_draw(lv_event_t *e)
{
	lv_layer_t *l = lv_event_get_layer(e);
	lv_area_t a;
	lv_obj_get_coords(p.cards, &a);
	if (p.prev_album)
		card(l, 1, W / 2.0 - PITCH + p.off, a.y1, p.prev_album, p.prev_cover);
	if (p.next_album)
		card(l, 2, W / 2.0 + PITCH + p.off, a.y1, p.next_album, p.next_cover);
}

void player_set_neighbors(const char *prev_album, const char *prev_cover, const char *next_album, const char *next_cover)
{
	p.prev_album = prev_album;
	p.next_album = next_album;
	p.prev_cover = prev_cover;
	p.next_cover = next_cover;
	lv_obj_invalidate(p.cards);
}

/* размер текущей обложки: уменьшается, уходя к месту соседа */
static int cover_size(void)
{
	return (int)(COVER - (COVER - CARD) * slot_k(W / 2.0 + p.off) + 0.5);
}

static void cover_apply(void)
{
	int sz = cover_size();
	const lv_image_dsc_t *src = p.cover_full ? scaled(0, p.cover_full, sz) : NULL;
	if (src && (lv_image_get_src(p.cover) != (const void *)src || src != p.cover_full))
		lv_image_set_src(p.cover, src);   /* копия того же адреса могла смениться размером — заголовок перечитать */
	lv_obj_set_pos(p.cover, COVER_X + (int)p.off + (COVER - sz) / 2, COVER_Y + (COVER - sz) / 2);
	lv_obj_set_size(p.cover, sz, sz);
}

/* обложка, пока её нет (декодируется или не поддержана) — кассета */
static void cover_refresh(void)
{
	const lv_image_dsc_t *d = cover_get(p.cover_path, COVER);
	int wait = !d && p.cover_path && cover_ready(p.cover_path, COVER) == 0;
	p.cover_full = d;
	if (d)
		cover_apply();
	if (lv_obj_is_hidden(p.cover) != !d)
		lv_obj_set_hidden(p.cover, !d);
	int lyr = p.lyr_k > 0.5;   /* под текстом песни — ни кассеты, ни заглушки */
	if (lv_obj_is_hidden(p.pending) != (!wait || lyr))
		lv_obj_set_hidden(p.pending, !wait || lyr);
	if (lv_obj_is_hidden(p.cassette) != (d || wait || lyr))
		lv_obj_set_hidden(p.cassette, d || wait || lyr);
	/* соседние альбомы — заранее в полном размере: при перелистывании обложка уже готова */
	cover_get(p.prev_cover, COVER);
	cover_get(p.next_cover, COVER);
	cover_get(p.prev_cover, CARD);
	cover_get(p.next_cover, CARD);
}

void player_set_cover(const char *path)
{
	p.cover_path = path;
	cover_refresh();
}

static void carousel_apply(void)
{
	int sz = cover_size();
	lv_obj_set_x(p.cassette, CAS_X + (int)p.off);
	if (p.cover_full)
		cover_apply();
	lv_obj_set_pos(p.pending, COVER_X + (int)p.off + (COVER - sz) / 2, COVER_Y + (COVER - sz) / 2);
	lv_obj_set_size(p.pending, sz, sz);
	lv_obj_invalidate(p.cards);
}

void player_drag(int dx)
{
	/* к краю без соседа — тугая резинка */
	int edge = (dx > 0 && !p.prev_album) || (dx < 0 && !p.next_album);
	p.dragging = 1;
	double v = dx * (edge ? 0.25 : 1.0);   /* за пальцем один к одному, но не дальше соседа */
	if (fabs(v) > PITCH)
		v = v > 0 ? PITCH : -PITCH;
	p.off = v;
	carousel_apply();
}

void player_release(int dir)
{
	p.dragging = 0;
	/* альбом уже сменился: новая обложка стоит там, где была карточка соседа, и доезжает до центра */
	if (dir)
		p.off += dir * PITCH;
	carousel_apply();
}

/* сердечко — глифы Font Awesome 4.7 (тот же набор, что встроенные символы LVGL): контур и залитое */
#define FA_HEART "\xEF\x80\x84"     /* U+F004 */
#define FA_HEART_O "\xEF\x82\x8A"   /* U+F08A */
#define FAV_RED 0xe8414a

void player_set_icon_font(const lv_font_t *f)
{
	lv_obj_set_style_text_font(p.heart, f, 0);
	lv_obj_set_style_text_font(p.mode_icon[PM_MORE], f, 0);
	lv_obj_set_style_text_font(p.mode_icon[PM_LYRICS], f, 0);
	p.fav = -1;
	player_set_fav(0);
}

void player_set_ab(double a, double b)
{
	double f[2] = { a, b };
	for (int i = 0; i < 2; i++) {
		lv_obj_set_hidden(p.ab[i], f[i] < 0);
		if (f[i] >= 0)
			lv_obj_set_x(p.ab[i], BAR_X + (int)(BAR_W * (f[i] > 1 ? 1 : f[i])) - 1);
	}
}

void player_set_fav(int on)
{
	if (p.fav != on) {
		p.fav = on;
		lv_label_set_text(p.heart, on ? FA_HEART : FA_HEART_O);
		lv_obj_set_style_text_color(p.heart, lv_color_hex(on ? FAV_RED : 0xaeb3bb), 0);
	}
}

int player_action_at(int x, int y)
{
	if (!p.shown || p.t < 1 || y < COVER_Y + COVER + 6 || y > ACT_Y + 14)
		return PA_NONE;
	if (y >= ACT_Y - 18 && (x < ACT_X + 28 || x > W - ACT_X - 28))
		return x < ACT_X + 28 ? PA_FAV : PA_ADD;
	return x >= ACT_X + 28 && x <= W - ACT_X - 28 ? PA_ALBUM : PA_NONE;
}

/* ---------- создание ---------- */

void player_create(lv_obj_t *parent, const lv_font_t *f_big, const lv_font_t *f_mid, const lv_font_t *f_small)
{
	/* плеер раскрывается из мини-плеера: та же полоса внизу растёт до полного экрана */
	p.root = box(parent, 0, H - MINI_H, W, MINI_H);
	lv_obj_set_style_bg_opa(p.root, MINI_OPA, 0);
	lv_obj_set_style_bg_color(p.root, lv_color_black(), 0);
	lv_obj_set_hidden(p.root, true);
	p.content = box(p.root, 0, 0, W, H);

	p.cards = box(p.content, 0, COVER_Y, W, COVER);
	lv_obj_add_event_cb(p.cards, cards_draw, LV_EVENT_DRAW_MAIN, NULL);
	p.cassette = box(p.content, CAS_X, CAS_Y, CAS_W, CAS_H + 28);
	lv_obj_add_event_cb(p.cassette, cassette_draw, LV_EVENT_DRAW_MAIN, NULL);
	p.pending = box(p.content, COVER_X, COVER_Y, COVER, COVER);
	lv_obj_set_style_bg_opa(p.pending, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(p.pending, lv_color_hex(EMPTY_BG), 0);
	lv_obj_set_hidden(p.pending, true);
	p.cover = lv_image_create(p.content);
	lv_obj_set_pos(p.cover, COVER_X, COVER_Y);
	lv_obj_set_size(p.cover, COVER, COVER);
	lv_obj_set_hidden(p.cover, true);

	p.lyr = box(p.content, 0, LYR_Y, W, LYR_H);   /* строки за краем обрезаются */
	p.lyr_font = f_mid;
	for (int i = 0; i < NLYR; i++) {
		p.lyr_line[i] = lv_label_create(p.lyr);
		lv_obj_set_style_text_font(p.lyr_line[i], f_mid, 0);
		lv_obj_set_style_text_color(p.lyr_line[i], lv_color_white(), 0);
		lv_obj_set_style_text_align(p.lyr_line[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_obj_set_width(p.lyr_line[i], W - 40);
		lv_label_set_long_mode(p.lyr_line[i], LV_LABEL_LONG_MODE_WRAP);
		lv_label_set_text(p.lyr_line[i], "");
		lv_obj_set_hidden(p.lyr_line[i], true);
		p.lyr_idx[i] = -1;
	}
	lv_obj_set_hidden(p.lyr, true);

	p.title = text(p.content, f_big, COVER_Y + COVER + 14, LV_OPA_COVER);
	p.artist = text(p.content, f_mid, COVER_Y + COVER + 38, 210);
	p.album = text(p.content, f_small, COVER_Y + COVER + 58, 140);
	lv_obj_set_width(p.album, W - 2 * (ACT_X + 28));
	lv_obj_set_x(p.album, ACT_X + 28);

	lv_obj_t *track = box(p.content, BAR_X, BAR_Y, BAR_W, 2);
	lv_obj_set_style_bg_color(track, lv_color_white(), 0);
	lv_obj_set_style_bg_opa(track, 50, 0);
	p.bar = box(p.content, BAR_X, BAR_Y - 1, 0, 4);
	lv_obj_set_style_radius(p.bar, 2, 0);
	lv_obj_set_style_bg_color(p.bar, lv_color_hex(ACCENT), 0);
	lv_obj_set_style_bg_opa(p.bar, LV_OPA_COVER, 0);
	for (int i = 0; i < 2; i++) {   /* отметки A и B */
		p.ab[i] = box(p.content, BAR_X, BAR_Y - 6, 2, 14);
		lv_obj_set_style_bg_color(p.ab[i], lv_color_white(), 0);
		lv_obj_set_style_bg_opa(p.ab[i], LV_OPA_COVER, 0);
		lv_obj_set_hidden(p.ab[i], true);
	}

	p.t_pos = lv_label_create(p.content);
	lv_obj_set_style_text_font(p.t_pos, f_small, 0);
	lv_obj_set_style_text_color(p.t_pos, lv_color_white(), 0);
	lv_obj_set_style_text_opa(p.t_pos, 170, 0);
	lv_obj_set_pos(p.t_pos, BAR_X, BAR_Y + 8);
	p.t_dur = lv_label_create(p.content);
	lv_obj_set_style_text_font(p.t_dur, f_small, 0);
	lv_obj_set_style_text_color(p.t_dur, lv_color_white(), 0);
	lv_obj_set_style_text_opa(p.t_dur, 170, 0);
	lv_obj_set_width(p.t_dur, 60);
	lv_obj_set_style_text_align(p.t_dur, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(p.t_dur, BAR_X + BAR_W - 60, BAR_Y + 8);

	p.format = text(p.content, f_small, BAR_Y + 30, 130);
	p.heart = lv_label_create(p.content);
	lv_obj_set_style_text_font(p.heart, &lv_font_montserrat_20, 0);
	lv_obj_set_width(p.heart, 32);
	lv_obj_set_style_text_align(p.heart, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_pos(p.heart, ACT_X - 16, ACT_Y - 11);
	p.fav = -1;
	player_set_fav(0);
	p.add = lv_label_create(p.content);
	lv_obj_set_style_text_font(p.add, &lv_font_montserrat_20, 0);
	lv_obj_set_style_text_color(p.add, lv_color_hex(0xaeb3bb), 0);
	lv_obj_set_width(p.add, 32);
	lv_obj_set_style_text_align(p.add, LV_TEXT_ALIGN_CENTER, 0);
	lv_label_set_text(p.add, LV_SYMBOL_PLUS);
	lv_obj_set_pos(p.add, W - ACT_X - 16, ACT_Y - 11);

	static const char *const icons[PM_COUNT] = { LV_SYMBOL_SHUFFLE, LV_SYMBOL_LOOP, FA_LYRICS, FA_MORE };
	for (int i = 0; i < PM_COUNT; i++) {
		int cx = MODE_STEP / 2 + i * MODE_STEP;
		p.mode_icon[i] = lv_label_create(p.content);
		lv_obj_set_style_text_font(p.mode_icon[i], &lv_font_montserrat_20, 0);
		lv_obj_set_style_text_color(p.mode_icon[i], lv_color_white(), 0);
		lv_obj_set_width(p.mode_icon[i], 60);
		lv_obj_set_style_text_align(p.mode_icon[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_text(p.mode_icon[i], icons[i]);
		lv_obj_set_pos(p.mode_icon[i], cx - 30, MODES_Y);
		p.mode_label[i] = lv_label_create(p.content);
		lv_obj_set_style_text_font(p.mode_label[i], f_small, 0);
		lv_obj_set_style_text_color(p.mode_label[i], lv_color_white(), 0);
		lv_obj_set_size(p.mode_label[i], MODE_STEP - 2, 16);
		lv_obj_set_style_text_align(p.mode_label[i], LV_TEXT_ALIGN_CENTER, 0);
		lv_label_set_long_mode(p.mode_label[i], LV_LABEL_LONG_MODE_DOTS);
		lv_label_set_text(p.mode_label[i], "");
		lv_obj_set_pos(p.mode_label[i], cx - MODE_STEP / 2 + 1, MODES_Y + 26);
		p.mode_dot[i] = box(p.content, cx - 8, MODES_Y + 46, 16, 3);
		lv_obj_set_style_radius(p.mode_dot[i], 1, 0);
		lv_obj_set_style_bg_color(p.mode_dot[i], lv_color_hex(ACCENT), 0);
		lv_obj_set_style_bg_opa(p.mode_dot[i], LV_OPA_COVER, 0);
	}

	/* громкость: сегменты, как на PSP */
	p.vol = box(p.content, 24, 30, W - 48, 34);
	lv_obj_set_style_radius(p.vol, 17, 0);
	lv_obj_set_style_bg_color(p.vol, lv_color_black(), 0);
	lv_obj_set_style_bg_opa(p.vol, 200, 0);
	lv_obj_t *vi = lv_label_create(p.vol);
	lv_obj_set_style_text_font(vi, &lv_font_montserrat_16, 0);
	lv_obj_set_style_text_color(vi, lv_color_white(), 0);
	lv_label_set_text(vi, LV_SYMBOL_VOLUME_MAX);
	lv_obj_set_pos(vi, 14, 8);
	p.vol_bar = box(p.vol, 44, 11, VOL_SEG * 6, 12);
	for (int i = 0; i < VOL_SEG; i++) {
		lv_obj_t *sgm = box(p.vol_bar, i * 6, 0, 4, 12);
		lv_obj_set_style_radius(sgm, 1, 0);
		lv_obj_set_style_bg_color(sgm, lv_color_white(), 0);
		lv_obj_set_style_bg_opa(sgm, 50, 0);
	}
	p.vol_text = lv_label_create(p.vol);
	lv_obj_set_style_text_font(p.vol_text, f_small, 0);
	lv_obj_set_style_text_color(p.vol_text, lv_color_white(), 0);
	lv_obj_set_width(p.vol_text, 30);
	lv_obj_set_style_text_align(p.vol_text, LV_TEXT_ALIGN_RIGHT, 0);
	lv_obj_set_pos(p.vol_text, W - 48 - 44, 10);
	p.volume = -1;
	lv_obj_set_hidden(p.vol, true);
}

void player_set_track(const track_t *t)
{
	char buf[16];
	p.track = t;
	lv_label_set_text(p.title, t->title);
	lv_label_set_text(p.artist, t->artist);
	lv_label_set_text(p.album, t->album);
	lv_label_set_text(p.format, t->format);
	fmt_time(buf, t->duration);
	lv_label_set_text(p.t_dur, buf);
	p.pos = -1;
}

void player_set_state(int playing, double pos, int volume)
{
	char buf[16];
	p.playing = playing;
	if (p.track && (int)pos != (int)p.pos) {
		fmt_time(buf, (int)pos);
		lv_label_set_text(p.t_pos, buf);
		lv_obj_set_width(p.bar, (int)(BAR_W * pos / p.track->duration));
		lv_obj_invalidate(p.cassette);   /* плёнка перематывается */
	}
	p.pos = pos;
	if (volume != p.volume) {
		p.volume = volume;
		lv_label_set_text_fmt(p.vol_text, "%d", volume);
		int lit = (volume * VOL_SEG + 50) / 100;
		for (int i = 0; i < VOL_SEG; i++)
			lv_obj_set_style_bg_opa(lv_obj_get_child(p.vol_bar, i), i < lit ? LV_OPA_COVER : 50, 0);
	}
}

void player_set_modes(const int on[PM_COUNT], const char *const label[PM_COUNT])
{
	for (int i = 0; i < PM_COUNT; i++) {
		lv_obj_set_style_text_opa(p.mode_icon[i], on[i] ? LV_OPA_COVER : 90, 0);
		lv_obj_set_style_text_opa(p.mode_label[i], on[i] ? 220 : 90, 0);
		lv_obj_set_hidden(p.mode_dot[i], !on[i]);
		if (strcmp(lv_label_get_text(p.mode_label[i]), label[i]))
			lv_label_set_text(p.mode_label[i], label[i]);
	}
}

int player_mode_at(int x, int y)
{
	if (!p.shown || y < MODES_Y - 12 || y > MODES_Y + 56)
		return -1;
	int i = x / MODE_STEP;
	return x >= 0 && i < PM_COUNT ? i : -1;
}

int player_bar_at(int x, int y, double *frac)
{
	if (!p.shown || p.t < 1 || y < BAR_Y - 9 || y > BAR_Y + 16 || x < BAR_X - 16 || x > BAR_X + BAR_W + 16)
		return 0;
	double f = (double)(x - BAR_X) / BAR_W;
	*frac = f < 0 ? 0 : f > 1 ? 1 : f;
	return 1;
}

double player_bar_frac(int x)
{
	double f = (double)(x - BAR_X) / BAR_W;
	return f < 0 ? 0 : f > 1 ? 1 : f;
}


/* высоты строк — один раз на текст: перенос по ширине; пустая строка — половина */
void player_set_lyrics(const lyrics_t *l)
{
	p.lyr_src = l && l->n ? l : NULL;
	free(p.lyr_y);
	p.lyr_y = NULL;
	p.lyr_total = 0;
	for (int i = 0; i < NLYR; i++)
		p.lyr_idx[i] = -1;
	if (p.lyr_src) {
		int lh = lv_font_get_line_height(p.lyr_font);
		p.lyr_y = malloc(sizeof(int) * (l->n + 1));
		for (int i = 0; i < l->n; i++) {
			lv_point_t sz;
			lv_text_get_size(&sz, l->v[i].text, p.lyr_font, 0, 2, W - 40, LV_TEXT_FLAG_NONE);
			p.lyr_y[i] = p.lyr_total;
			p.lyr_total += (l->v[i].text[0] ? sz.y : lh / 2) + 10;
		}
		p.lyr_y[l->n] = p.lyr_total;
	}
	p.lyr_cur = -2;
	p.lyr_scroll = 0;
}

void player_show_lyrics(int on)
{
	p.lyr_on = on;
}

int player_lyrics_shown(void)
{
	return p.lyr_on;
}

/* строки вокруг текущей: позиция — от середины области, текущая яркая, дальние гаснут */
static void lyrics_layout(void)
{
	const lyrics_t *l = p.lyr_src;
	int cur = !l ? -1 : l->synced ? lyrics_at(l, p.pos + 0.3) : -1;
	double target;
	if (!l)
		target = 0;
	else if (l->synced)
		target = cur < 0 ? 0 : p.lyr_y[cur] + (p.lyr_y[cur + 1] - p.lyr_y[cur]) / 2.0;
	else   /* без меток — едет по ходу трека */
		target = p.track && p.track->duration > 0 ? p.lyr_total * p.pos / p.track->duration : 0;
	if (p.lyr_cur == -2)
		p.lyr_scroll = target;
	p.lyr_scroll += (target - p.lyr_scroll) * 0.18;
	if (fabs(target - p.lyr_scroll) < 0.5)
		p.lyr_scroll = target;
	p.lyr_cur = cur;
	int first = 0;
	if (l)
		while (first < l->n - 1 && p.lyr_y[first + 1] < p.lyr_scroll - LYR_H / 2)
			first++;
	for (int k = 0; k < NLYR; k++) {
		int i = first + k;
		lv_obj_t *o = p.lyr_line[i % NLYR];
		if (!l || i >= l->n) {   /* NLYR подряд идущих строк занимают все слоты ровно по разу */
			p.lyr_idx[i % NLYR] = -1;
			if (!lv_obj_is_hidden(o))
				lv_obj_set_hidden(o, true);
			continue;
		}
		if (p.lyr_idx[i % NLYR] != i) {
			p.lyr_idx[i % NLYR] = i;
			lv_label_set_text(o, l->v[i].text);
		}
		int y = (int)(p.lyr_y[i] - p.lyr_scroll + LYR_H / 2.0);
		if (lv_obj_get_y(o) != y || lv_obj_get_x(o) != 20)
			lv_obj_set_pos(o, 20, y);
		double d = fabs(p.lyr_y[i] + (p.lyr_y[i + 1] - p.lyr_y[i]) / 2.0 - p.lyr_scroll) / (LYR_H / 2.0);
		int opa = i == cur ? 255 : (int)(150 * (1.0 - fmin(1.0, d * d)) + 20);
		if (!l->synced)
			opa = (int)(230 * (1.0 - fmin(1.0, d * d * 0.6)) + 25);
		opa = (int)(opa * p.lyr_k);
		if (lv_obj_get_style_text_opa(o, 0) != opa)
			lv_obj_set_style_text_opa(o, (lv_opa_t)opa, 0);
		if (lv_obj_is_hidden(o))
			lv_obj_set_hidden(o, false);
	}
}

void player_show_volume(void)
{
	p.vol_t = 1.4;
	lv_obj_set_hidden(p.vol, false);
	lv_obj_set_style_opa(p.vol, LV_OPA_COVER, 0);
}

void player_show(int show)
{
	p.shown = show;
}

int player_shown(void)
{
	return p.shown;
}

int player_covers(void)
{
	return p.shown && p.t >= 1;
}

int player_carousel_moving(void)
{
	return p.off != 0;
}

double player_progress(void)
{
	return p.t;
}

void player_tick(double dt)
{
	double target = p.shown ? 1 : 0;
	if (p.t != target) {
		p.t += (target - p.t) * (1.0 - exp(-dt * 12));
		if (fabs(target - p.t) < 0.004)
			p.t = target;
		int y = (int)((H - MINI_H) * (1.0 - p.t));
		lv_obj_set_pos(p.root, 0, y);
		lv_obj_set_height(p.root, H - y);
		/* раскрыт — затемнение уже в самом фоне (bg_set_dim) */
		lv_obj_set_style_bg_opa(p.root, p.t >= 1 ? 0 : (lv_opa_t)(MINI_OPA + (FULL_OPA - MINI_OPA) * p.t), 0);
		/* прозрачность содержимого — только на время перехода: она рендерится через слой */
		lv_obj_set_style_opa(p.content, p.t >= 1 ? LV_OPA_COVER : (lv_opa_t)(255 * p.t * p.t), 0);
		lv_obj_set_hidden(p.root, p.t <= 0);
	}
	if (p.t > 0) {
		cover_refresh();   /* опрос держит обложку в кэше */
		if (p.cover_gen != cover_generation()) {   /* дорисовались обложки соседей */
			p.cover_gen = cover_generation();
			lv_obj_invalidate(p.cards);
		}
	}
	if (!p.dragging && p.off != 0) {
		p.off += (0 - p.off) * (1.0 - exp(-dt * 12));
		if (fabs(p.off) < 0.5)
			p.off = 0;
		carousel_apply();
	}
	/* катушки: 25 обновлений в секунду, перерисовывается только окошко */
	if (p.shown && p.playing && !lv_obj_is_hidden(p.cassette)) {
		p.acc += dt;
		if (p.acc >= 0.04) {
			p.angle += p.acc * 2.4;
			p.acc = 0;
			lv_area_t a;
			lv_obj_get_coords(p.cassette, &a);
			lv_area_t win = { a.x1 + 40, a.y1 + 26, a.x2 - 40, a.y1 + 90 };
			lv_obj_invalidate_area(p.cassette, &win);
		}
	}
	/* текст вместо обложки: обложка гаснет, строки проявляются */
	double lk = p.lyr_on && p.lyr_src ? 1 : 0;
	if (p.lyr_k != lk) {
		p.lyr_k += (lk - p.lyr_k) * (1.0 - exp(-dt * 12));
		if (fabs(lk - p.lyr_k) < 0.01)
			p.lyr_k = lk;
		lv_obj_set_style_image_opa(p.cover, (lv_opa_t)(255 * (1 - p.lyr_k)), 0);
		lv_obj_set_hidden(p.cards, p.lyr_k >= 1);   /* соседи гаснут в cards_draw */
		lv_obj_invalidate(p.cards);
		lv_obj_set_hidden(p.lyr, p.lyr_k == 0);
	}
	if (p.t > 0 && p.lyr_k > 0)
		lyrics_layout();
	if (p.vol_t > 0) {
		p.vol_t -= dt;
		if (p.vol_t <= 0)
			lv_obj_set_hidden(p.vol, true);
		else if (p.vol_t < 0.3)
			lv_obj_set_style_opa(p.vol, (lv_opa_t)(255 * p.vol_t / 0.3), 0);
	}
}
