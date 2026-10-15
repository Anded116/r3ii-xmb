/* Частицы: редкие мягкие «пузырьки света» разного размера неспешно всплывают снизу вверх, слегка покачиваются
 * и мерцают; снизу плавно проявляются, у верха полосы тают. Громкость ускоряет подъём и делает ярче, спектр
 * (басы слева) — местная яркость, удар бочки — короткая вспышка у всех.
 *
 * Положение частицы — с суб-пиксельной точностью: спрайты (альфа по радиусу) заранее посчитаны в init для
 * каждого класса радиуса и для 4×4 дробных смещений центра, поэтому движение плавное, без дрожи по пикселям.
 * Перерисовка: у каждой частицы своё нарисованное состояние (спрайт, целая позиция, яркость). Если оно
 * поменялось — в dirty уходит один прямоугольник «старый габарит + новый». В прямоугольнике фон
 * восстанавливается и заново рисуются все задевающие его частицы (порядок — по номеру). */
#include <math.h>
#include <string.h>

#include "wave.h"

#define NP 24           /* частиц */
#define NCLS 9          /* классов радиуса: 2.0, 2.5 … 6.0 */
#define NPH 16          /* дробных смещений центра: 4 по x × 4 по y */
#define TAB_N 12624     /* сумма квадратов окон классов × NPH */
#define SIN_N 1024
#define TURN 0xFFFFF    /* фаза: полный оборот = 2^20 */
#define FADE_IN 7       /* крутизна проявления снизу, 256 = за 36 px */
#define FADE_OUT 5      /* крутизна таяния у верха */

typedef struct {
	int32_t y;              /* центр по y, Q16, растёт вниз */
	int32_t x0;             /* опорный x, Q8 */
	uint32_t wph, wf;       /* фаза и частота покачивания */
	uint32_t fph, ff;       /* фаза и частота мерцания */
	uint16_t v;             /* скорость подъёма, px/с в Q4 */
	uint16_t amp;           /* размах покачивания, px в Q4 */
	uint8_t cls, bq;        /* класс радиуса, базовая яркость */
} par_t;

typedef struct {
	int16_t x1, y1, x2, y2; /* окно спрайта, включительно */
	uint16_t off;           /* смещение в tab */
	uint8_t sw;             /* сторона окна */
	uint8_t lv;             /* яркость 0..64, 0 — не рисуется */
} st_t;

static uint8_t tab[TAB_N];
static uint16_t cls_off[NCLS];
static uint8_t cls_s[NCLS], cls_l[NCLS];
static int16_t sintab[SIN_N];
static par_t par[NP];
static st_t st[NP];
static uint32_t rng;
static int spawn_x;
static int lvs, ebase, flash, started;
static int spec8[WAVE_BANDS + 1];
static int gbase, spdq;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void respawn(par_t *p, int y_px)
{
	static const uint8_t cmap[16] = { 0, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8 };
	p->cls = cmap[rnd() & 15];
	spawn_x = (spawn_x + 197 + (int)(rnd() % 40) - 20) % (WAVE_W - 24);
	p->x0 = (spawn_x + 12) << 8;
	p->y = y_px << 16;
	p->v = (uint16_t)((7 * 16) + p->cls * 8 + rnd() % 80);
	p->amp = (uint16_t)(3 * 16 + rnd() % (6 * 16));
	p->wph = rnd() & TURN;
	p->wf = 50000 + rnd() % 110000;
	p->fph = rnd() & TURN;
	p->ff = 450000 + rnd() % 650000;
	p->bq = (uint8_t)(170 + rnd() % 86);
}

static void init(void)
{
	rng = 2463534242u;
	for (int i = 0; i < SIN_N; i++)
		sintab[i] = (int16_t)lrint(4096 * sin(2 * M_PI * i / SIN_N));

	int off = 0;
	for (int c = 0; c < NCLS; c++) {
		double R = 2 + 0.5 * c;
		int L = (int)(R + 0.5), H = (int)(R + 0.25), S = L + H + 1;
		double peak = 1.0 - 0.025 * c;
		cls_off[c] = (uint16_t)off;
		cls_s[c] = (uint8_t)S;
		cls_l[c] = (uint8_t)L;
		for (int ph = 0; ph < NPH; ph++) {
			double fx = (ph & 3) / 4.0, fy = (ph >> 2) / 4.0;
			for (int j = 0; j < S; j++)
				for (int i = 0; i < S; i++) {
					double dx = i - L + 0.5 - fx, dy = j - L + 0.5 - fy;
					double t = sqrt(dx * dx + dy * dy) / R, a = 0;
					if (t < 1) {
						double s = 1 - t * t * (3 - 2 * t);
						a = peak * (0.7 * s + 0.3 * (1 - t) * (1 - t));
					}
					tab[off++] = (uint8_t)lrint(a * 255);
				}
		}
	}

	spawn_x = 0;
	for (int i = 0; i < NP; i++) {
		respawn(&par[i], 0);
		par[i].y = (int32_t)((rnd() % (WAVE_H + 20)) - 10) << 16;
	}
	lvs = ebase = flash = started = 0;
}

static inline int clampi(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

/* состояние частицы i в текущий момент */
static void calc(int i, st_t *s)
{
	const par_t *p = &par[i];
	int y8 = p->y >> 8;
	int in = clampi(((WAVE_H + 2) * 256 - y8) * FADE_IN >> 8, 0, 256);
	int out = clampi((y8 - 4 * 256) * FADE_OUT >> 8, 0, 256);
	in = in * in >> 8;
	out = out * out >> 8;
	int env = in * out >> 8;
	s->lv = 0;
	s->off = 0;
	s->sw = 0;
	s->x1 = s->y1 = s->x2 = s->y2 = 0;
	if (!env)
		return;

	int x8 = p->x0 + ((sintab[(p->wph >> 10) & (SIN_N - 1)] * p->amp) >> 8);
	int cxq = x8 >> 6, cyq = p->y >> 14;
	int xi = clampi(x8 >> 8, 0, WAVE_W - 1);
	int ib = xi >> 3, fr = xi & 7;
	int sv = (spec8[ib] * (8 - fr) + spec8[ib + 1] * fr) >> 3;

	int fl = 200 + ((sintab[(p->fph >> 10) & (SIN_N - 1)] * 56) >> 12);
	int q = p->bq * fl >> 8;
	q = q * env >> 8;
	q = q * (gbase + (sv * 80 >> 8)) >> 8;
	int lv = clampi(q >> 2, 0, 64);
	if (!lv)
		return;

	int c = p->cls, S = cls_s[c];
	int ph = ((cyq & 3) << 2) | (cxq & 3);
	s->sw = (uint8_t)S;
	s->off = (uint16_t)(cls_off[c] + ph * S * S);
	s->x1 = (int16_t)((cxq >> 2) - cls_l[c]);
	s->y1 = (int16_t)((cyq >> 2) - cls_l[c]);
	s->x2 = (int16_t)(s->x1 + S - 1);
	s->y2 = (int16_t)(s->y1 + S - 1);
	s->lv = (uint8_t)lv;
}

/* осветление пикселя RGB565 к белому на a/32 (a 0..32), три канала за раз */
static inline uint16_t blend(uint16_t p, int a)
{
	const uint32_t M = 0x07E0F81Fu, RN = 0x02008010u;
	uint32_t x = (p | ((uint32_t)p << 16)) & M;
	x += ((((M - x) * (uint32_t)a) + RN) >> 5) & M;
	return (uint16_t)(x | (x >> 16));
}

/* нарисовать частицу, обрезав по прямоугольнику */
static void draw(uint16_t *dst, const st_t *s, int rx1, int ry1, int rx2, int ry2)
{
	int x1 = s->x1 > rx1 ? s->x1 : rx1, x2 = s->x2 < rx2 ? s->x2 : rx2;
	int y1 = s->y1 > ry1 ? s->y1 : ry1, y2 = s->y2 < ry2 ? s->y2 : ry2;
	int lv = s->lv;
	for (int y = y1; y <= y2; y++) {
		const uint8_t *t = tab + s->off + (y - s->y1) * s->sw + (x1 - s->x1);
		uint16_t *q = dst + y * WAVE_W + x1;
		for (int x = x1; x <= x2; x++, t++, q++) {
			int a = (*t * lv + 256) >> 9;
			if (a)
				*q = blend(*q, a);
		}
	}
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	int dq = clampi((int)(f->dt * 16384.0), 0, 983);
	int lv = clampi((int)(f->level * 256.0f), 0, 256);
	lvs += (lv - lvs) >> 3;
	int e = 0;
	for (int b = 0; b < WAVE_BANDS; b++) {
		spec8[b] = clampi((int)(f->spec[b] * 256.0f), 0, 256);
		if (b < 5)
			e += spec8[b];
	}
	spec8[WAVE_BANDS] = spec8[WAVE_BANDS - 1];
	e /= 5;
	int k = (e - ebase - 20) * 3 / 2;
	ebase += (e - ebase) >> 4;
	flash = k > flash ? (k > 200 ? 200 : k) : flash * 236 >> 8;
	gbase = 250 + (lvs * 70 >> 8) + (flash * 80 >> 8);
	spdq = (256 + (lvs * 200 >> 8)) * dq;

	/* движение */
	for (int i = 0; i < NP; i++) {
		par_t *p = &par[i];
		p->y -= (int32_t)(p->v * spdq >> 10);
		p->wph = (p->wph + (uint32_t)((uint64_t)p->wf * dq >> 14)) & TURN;
		p->fph = (p->fph + (uint32_t)((uint64_t)p->ff * dq >> 14)) & TURN;
		if (p->y < -(8 << 16))
			respawn(p, WAVE_H + 8);
	}

	if (f->full || !started) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		for (int i = 0; i < NP; i++) {
			calc(i, &st[i]);
			if (st[i].lv)
				draw(dst, &st[i], 0, 0, WAVE_W - 1, WAVE_H - 1);
		}
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		started = 1;
		return;
	}

	wave_rect_t rc[NP];
	int nr = 0;
	for (int i = 0; i < NP; i++) {
		st_t n, *o = &st[i];
		calc(i, &n);
		if (!n.lv && !o->lv) {
			*o = n;
			continue;
		}
		if (n.lv == o->lv && n.off == o->off && n.x1 == o->x1 && n.y1 == o->y1)
			continue;
		wave_rect_t r;
		if (o->lv && n.lv)
			r = (wave_rect_t){ o->x1 < n.x1 ? o->x1 : n.x1, o->y1 < n.y1 ? o->y1 : n.y1,
					   o->x2 > n.x2 ? o->x2 : n.x2, o->y2 > n.y2 ? o->y2 : n.y2 };
		else if (o->lv)
			r = (wave_rect_t){ o->x1, o->y1, o->x2, o->y2 };
		else
			r = (wave_rect_t){ n.x1, n.y1, n.x2, n.y2 };
		*o = n;
		r.x1 = r.x1 < 0 ? 0 : r.x1;
		r.y1 = r.y1 < 0 ? 0 : r.y1;
		r.x2 = r.x2 > WAVE_W - 1 ? WAVE_W - 1 : r.x2;
		r.y2 = r.y2 > WAVE_H - 1 ? WAVE_H - 1 : r.y2;
		if (r.x1 <= r.x2 && r.y1 <= r.y2)
			rc[nr++] = r;
	}

	for (int j = 0; j < nr; j++) {
		wave_rect_t r = rc[j];
		for (int y = r.y1; y <= r.y2; y++)
			memcpy(dst + y * WAVE_W + r.x1, bg + y * WAVE_W + r.x1, sizeof(uint16_t) * (r.x2 - r.x1 + 1));
		for (int i = 0; i < NP; i++) {
			const st_t *s = &st[i];
			if (s->lv && s->x1 <= r.x2 && s->x2 >= r.x1 && s->y1 <= r.y2 && s->y2 >= r.y1)
				draw(dst, s, r.x1, r.y1, r.x2, r.y2);
		}
		wave_dirty_add(dirty, r.x1, r.y1, r.x2, r.y2);
	}
}

const wave_t wave_particles = { "Частицы", init, render };
