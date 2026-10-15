/*
 * Фон XMB: градиент темы в RGB565 с упорядоченным дизерингом (без полос) и волна.
 *
 * Волна — сменный модуль (src/waves, интерфейс wave.h): каждый кадр рисует полосу RGB565 поверх фона
 * и отдаёт прямоугольники, которые изменились; LVGL перерисовывает только их (и то, что лежит поверх).
 * Спектр звука раскладывается по ширине экрана, басы слева. В плеере полоса опускается под обложку.
 */
#include "xmb_bg.h"
#include "waves/wave.h"

#include <math.h>
#include <string.h>

#define W 320
#define H 480
#define BAND_Y 214          /* полоса волны в меню */
#define BAND_Y_PLAYER 270   /* в плеере: сразу под обложкой */
#define BAND_H 152
#define NSPEC 32
_Static_assert(WAVE_Y == BAND_Y && WAVE_H == BAND_H && WAVE_W == W && WAVE_BANDS == NSPEC, "полоса модулей волны");

extern const wave_t wave_ps3, wave_ribbon, wave_particles, wave_spectrum, wave_silk, wave_aurora, wave_dust,
	wave_dust_smooth, wave_pulse, wave_ripple, wave_psp;
/* по порядку bg_wave_t */
static const wave_t *const modules[BG_WAVE_OFF] = {
	&wave_ps3, &wave_ribbon, &wave_particles, &wave_spectrum, &wave_silk,
	&wave_aurora, &wave_dust, &wave_dust_smooth, &wave_pulse, &wave_ripple, &wave_psp,
};

static uint16_t pixels[W * H];
static uint16_t dimmed[W * H];   /* фон, уже затемнённый: под раскрытым плеером не смешивать затемнение каждый кадр */
static uint16_t band[W * BAND_H];
static lv_image_dsc_t img, dim_img, band_img;
static lv_obj_t *image, *wave;
static bg_wave_t style = BG_WAVE_COUNT;
static float spec[NSPEC], level;
static int band_y = BAND_Y;
static int full = 1;        /* модулю — перерисовать всё (новый фон, стиль, место полосы) */
static int dim, dim_done;   /* затемнение и для какого оно посчитано (0 — не посчитано) */
static double t_sec;

static const wave_t *module(void)
{
	return style < BG_WAVE_OFF ? modules[style] : NULL;
}

/* матрица Байера 8×8, значения 0..63 */
static const uint8_t bayer[8][8] = {
	{ 0, 32, 8, 40, 2, 34, 10, 42 }, { 48, 16, 56, 24, 50, 18, 58, 26 },
	{ 12, 44, 4, 36, 14, 46, 6, 38 }, { 60, 28, 52, 20, 62, 30, 54, 22 },
	{ 3, 35, 11, 43, 1, 33, 9, 41 }, { 51, 19, 59, 27, 49, 17, 57, 25 },
	{ 15, 47, 7, 39, 13, 45, 5, 37 }, { 63, 31, 55, 23, 61, 29, 53, 21 },
};

void bg_set_colors(uint32_t top, uint32_t bottom)
{
	/* мягкое светлое пятно сверху слева, как в XMB: не зависит от темы, считается один раз */
	static uint8_t glow[W * H];
	static int glow_ready;
	if (!glow_ready) {
		for (int y = 0; y < H; y++)
			for (int x = 0; x < W; x++) {
				double dx = (x - 90) / 260.0, dy = (y - 120) / 300.0;
				glow[y * W + x] = (uint8_t)(255 * exp(-(dx * dx + dy * dy) * 2.2));
			}
		glow_ready = 1;
	}
	/* целочисленно (предпросмотр темы пересчитывает фон на каждом шаге по списку): канал в 1/65536 долях,
	 * свечение добавляет до 0.10; уровень = канал × (уровней − 1) + дизеринг (Байер 8×8) */
	int t[3] = { (top >> 16) & 255, (top >> 8) & 255, top & 255 };
	int b[3] = { (bottom >> 16) & 255, (bottom >> 8) & 255, bottom & 255 };
	static const int levels[3] = { 32, 64, 32 };
	for (int y = 0; y < H; y++) {
		int row[3];
		for (int i = 0; i < 3; i++)
			row[i] = (int)(((int64_t)(t[i] * (H - 1) + (b[i] - t[i]) * y) << 16) / (255 * (H - 1)));
		for (int x = 0; x < W; x++) {
			int g = glow[y * W + x] * 6554 / 255;   /* 0.10 × 65536 */
			int d = (bayer[y & 7][x & 7] * 2 + 1) * 512;   /* (bayer + 0.5) / 64 в 1/65536 */
			int c[3];
			for (int i = 0; i < 3; i++) {
				int v = row[i] + g;
				v = v > 65536 ? 65536 : v;
				int q = (v * (levels[i] - 1) + d) >> 16;
				c[i] = q >= levels[i] ? levels[i] - 1 : q;
			}
			pixels[y * W + x] = (uint16_t)((c[0] << 11) | (c[1] << 5) | c[2]);
		}
	}
	full = 1;
	dim_done = 0;
	if (dim)
		bg_set_dim(dim);
	if (image)
		lv_obj_invalidate(image);
}

/* та же формула, что у LVGL для заливки с прозрачностью (LV_COLOR_MIX_16_TO_16_PREPARED): вид — как у слоя поверх */
static void make_dimmed(int opa)
{
	uint32_t mix5 = ((uint32_t)opa + 4) >> 3;
	for (int i = 0; i < W * H; i++) {
		uint32_t bg = ((uint32_t)pixels[i] | ((uint32_t)pixels[i] << 16)) & 0x07E0F81Fu;
		uint32_t r = ((((0 - bg) * mix5) >> 5) + bg) & 0x07E0F81Fu;
		dimmed[i] = (uint16_t)((r >> 16) | r);
	}
	dim_done = opa;
}

void bg_set_dim(int opa)
{
	if (opa && dim_done != opa)
		make_dimmed(opa);
	if (opa == dim && image && lv_image_get_src(image) == (const void *)(opa ? &dim_img : &img))
		return;
	dim = opa;
	lv_image_set_src(image, opa ? &dim_img : &img);
	if (wave)
		lv_obj_invalidate(wave);
}

static void draw_cb(lv_event_t *e)
{
	if (!module())
		return;
	lv_draw_image_dsc_t d;
	lv_draw_image_dsc_init(&d);
	d.src = &band_img;
	lv_area_t a = { 0, band_y, W - 1, band_y + BAND_H - 1 };
	lv_draw_image(lv_event_get_layer(e), &d, &a);
	if (dim) {   /* полоса рисуется по незатемнённому фону — затемнить её, как всё остальное */
		lv_draw_rect_dsc_t r;
		lv_draw_rect_dsc_init(&r);
		r.bg_color = lv_color_black();
		r.bg_opa = (lv_opa_t)dim;
		lv_draw_rect(lv_event_get_layer(e), &r, &a);
	}
}

/* полоса волны: в меню — посередине экрана, в плеере — под обложкой (верх экрана занят ею) */
void bg_set_band(int player)
{
	int y = player ? BAND_Y_PLAYER : BAND_Y;
	if (y == band_y)
		return;
	lv_area_t a = { 0, band_y, W - 1, band_y + BAND_H - 1 };
	lv_obj_invalidate_area(wave, &a);
	band_y = y;
	full = 1;
}

void bg_set_level(double l)
{
	level = (float)(l < 0 ? 0 : l > 1 ? 1 : l);
}

void bg_set_spectrum(const float *v, int n)
{
	for (int i = 0; i < NSPEC; i++)
		spec[i] = i < n ? v[i] : 0;
}

/* мягкое белое пятно свечения size×size (ARGB8888), строится один раз на размер */
const lv_image_dsc_t *bg_glow(int size)
{
	static lv_image_dsc_t dsc[4];
	static int sizes[4], n;
	for (int i = 0; i < n; i++)
		if (sizes[i] == size)
			return &dsc[i];
	if (n == 4)
		return &dsc[0];
	uint32_t *b = lv_malloc(size * size * 4);
	for (int y = 0; y < size; y++)
		for (int x = 0; x < size; x++) {
			double dx = (x + 0.5) / size * 2 - 1, dy = (y + 0.5) / size * 2 - 1;
			double a = exp(-(dx * dx + dy * dy) * 3.2) - exp(-3.2);
			int ai = (int)(255 * fmax(0.0, a / (1 - exp(-3.2))));
			b[y * size + x] = ((uint32_t)ai << 24) | 0xffffff;
		}
	lv_image_dsc_t *d = &dsc[n];
	memset(d, 0, sizeof(*d));
	d->header.magic = LV_IMAGE_HEADER_MAGIC;
	d->header.cf = LV_COLOR_FORMAT_ARGB8888;
	d->header.w = size;
	d->header.h = size;
	d->header.stride = size * 4;
	d->data_size = size * size * 4;
	d->data = (const uint8_t *)b;
	sizes[n++] = size;
	return d;
}

void bg_set_wave(bg_wave_t w)
{
	if (w == style)
		return;
	style = w;
	if (module())
		module()->init();
	full = 1;
	lv_obj_set_hidden(wave, w == BG_WAVE_OFF);
	lv_obj_invalidate(wave);
}

static int rect_area(wave_rect_t r)
{
	return (r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1);
}

static wave_rect_t rect_union(wave_rect_t a, wave_rect_t b)
{
	return (wave_rect_t){ a.x1 < b.x1 ? a.x1 : b.x1, a.y1 < b.y1 ? a.y1 : b.y1,
			      a.x2 > b.x2 ? a.x2 : b.x2, a.y2 > b.y2 ? a.y2 : b.y2 };
}

/* каждая область перерисовки — обход всего дерева объектов LVGL (сотни объектов), это дороже самих пикселей:
 * склеиваем прямоугольники попарно с наименьшим приростом площади, пока их не останется max */
static void merge_dirty(wave_dirty_t *d, int max)
{
	while (d->n > max) {
		int bi = 0, bj = 1, best = 1 << 30;
		for (int i = 0; i < d->n; i++)
			for (int j = i + 1; j < d->n; j++) {
				int g = rect_area(rect_union(d->r[i], d->r[j])) - rect_area(d->r[i]) - rect_area(d->r[j]);
				if (g < best) {
					best = g;
					bi = i;
					bj = j;
				}
			}
		d->r[bi] = rect_union(d->r[bi], d->r[bj]);
		d->r[bj] = d->r[--d->n];
	}
}

/* каждый кадр; на экран — только изменившиеся прямоугольники полосы */
void bg_tick(double dt)
{
	if (!module())
		return;
	t_sec += dt;
	wave_frame_t f = { t_sec, dt, spec, level, full };
	wave_dirty_t d = { 0 };
	module()->render(band, pixels + band_y * W, &f, &d);
	merge_dirty(&d, 2);
	if (full) {
		lv_area_t a = { 0, band_y, W - 1, band_y + BAND_H - 1 };
		lv_obj_invalidate_area(wave, &a);
	}
	full = 0;
	for (int i = 0; i < d.n; i++) {
		lv_area_t a = { d.r[i].x1, band_y + d.r[i].y1, d.r[i].x2, band_y + d.r[i].y2 };
		lv_obj_invalidate_area(wave, &a);
	}
}

void bg_create(lv_obj_t *parent)
{
	memset(&img, 0, sizeof(img));
	img.header.magic = LV_IMAGE_HEADER_MAGIC;
	img.header.cf = LV_COLOR_FORMAT_RGB565;
	img.header.w = W;
	img.header.h = H;
	img.header.stride = W * 2;
	img.data_size = sizeof(pixels);
	img.data = (const uint8_t *)pixels;
	dim_img = img;
	dim_img.data = (const uint8_t *)dimmed;
	image = lv_image_create(parent);
	lv_image_set_src(image, &img);
	lv_obj_set_pos(image, 0, 0);

	band_img = img;
	band_img.header.h = BAND_H;
	band_img.data_size = sizeof(band);
	band_img.data = (const uint8_t *)band;

	/* объект на весь экран без стиля: только своя отрисовка поверх фона */
	wave = lv_obj_create(parent);
	lv_obj_remove_style_all(wave);
	lv_obj_set_scrollable(wave, false);
	lv_obj_set_clickable(wave, false);
	lv_obj_set_size(wave, W, H);
	lv_obj_set_pos(wave, 0, 0);
	lv_obj_add_event_cb(wave, draw_cb, LV_EVENT_DRAW_MAIN, NULL);
}
