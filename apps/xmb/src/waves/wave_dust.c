/* Звёздная пыль: светящиеся частицы текут слева направо вдоль невидимой плавной волны (лента), мерцают,
 * у части — гаснущие хвостики вдоль траектории. Басы (spec[0..4]) дают всплеск: частицы ярче, шире
 * разлетаются от линии, хвосты длиннее; спектр по ширине (басы слева) — местная яркость.
 *
 * Перерисовка: у каждой частицы своё нарисованное состояние (целый x, y, уровень яркости, хвост), и
 * обновляется оно не каждый кадр, а раз в «период» кадров (крупные звёзды — чаще, мелкая пыль — реже,
 * на ударах — вдвое чаще): частица прыгает на накопленный сдвиг и берёт новую яркость мерцания. Старый и
 * новый габарит обновлённой частицы — один маленький прямоугольник; близкие прямоугольники сливаются,
 * пока лишняя площадь мала, а если их больше WAVE_MAX_DIRTY — сливаются те, у кого лишней площади меньше
 * всего. В каждом прямоугольнике фон восстанавливается и заново рисуются все частицы, задевающие его
 * (порядок — по номеру). */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define NP 160          /* частиц */
#define MARG 12         /* поле за краями, где частицы невидимы */
#define XR ((WAVE_W + 2 * MARG) * 16)
#define CY (WAVE_H / 2)
#define SIN_N 1024
#define NR 40           /* прямоугольников до слияния */
#define SLACK 16        /* слить два прямоугольника, если лишняя площадь не больше */

typedef struct {
	uint16_t x0;            /* начальный x, 1/16 px */
	uint16_t v;             /* скорость относительно потока, 256 = базовая */
	int16_t lane;           /* смещение от линии ленты */
	uint8_t sz, tail, b0, tsp, tph, wsp, wph, per;
} par_t;

typedef struct {
	int16_t ix, iy;
	uint8_t lv, tl;         /* яркость 0..15, длина хвоста */
	int8_t ts;              /* наклон хвоста */
} st_t;

typedef struct {
	signed char dx, dy;
	unsigned char w;        /* вес, 64 = 1 */
} sp_t;

static const sp_t sp0[] = { { 0, 0, 64 } };
static const sp_t sp1[] = { { 0, 0, 64 }, { -1, 0, 26 }, { 1, 0, 26 }, { 0, -1, 26 }, { 0, 1, 26 } };
static const sp_t sp2[] = {
	{ 0, 0, 64 }, { -1, 0, 42 }, { 1, 0, 42 }, { 0, -1, 42 }, { 0, 1, 42 },
	{ -1, -1, 20 }, { 1, -1, 20 }, { -1, 1, 20 }, { 1, 1, 20 },
	{ -2, 0, 12 }, { 2, 0, 12 }, { 0, -2, 12 }, { 0, 2, 12 },
};
static const sp_t *const spr[3] = { sp0, sp1, sp2 };
static const int spn[3] = { 1, 5, 13 };
static const int rad[3] = { 0, 1, 2 };
static const int tailw[6] = { 36, 26, 18, 12, 8, 5 };
static const int perz[3] = { 7, 5, 3 };   /* период обновления по размеру, кадров */

static int16_t sintab[SIN_N];
static par_t par[NP];
static st_t st[NP];
static uint8_t age[NP];
static int16_t bx1[NP], by1[NP], bx2[NP], by2[NP];
static uint32_t flow16, flow_s, rng;
static int lvs, ebase, burst, started, frame_n;
static int spec8[WAVE_BANDS];
static int amp1, amp2, p1, p2, p3;
static uint32_t tq;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static void init(void)
{
	rng = 2463534242u;
	for (int i = 0; i < SIN_N; i++)
		sintab[i] = (int16_t)lrint(1024 * sin(2 * M_PI * i / SIN_N));
	for (int i = 0; i < NP; i++) {
		par_t *p = &par[i];
		int r = rnd() % 100;
		p->sz = r < 40 ? 0 : r < 80 ? 1 : 2;
		p->v = p->sz == 0 ? 110 + rnd() % 100 : p->sz == 1 ? 180 + rnd() % 100 : 240 + rnd() % 140;
		p->b0 = p->sz == 0 ? 150 + rnd() % 80 : p->sz == 1 ? 190 + rnd() % 50 : 230 + rnd() % 26;
		int l = (int)(rnd() % 128) + (int)(rnd() % 128) - 127;
		if (rnd() % 100 < 12)
			l = l * 22 / 10;
		p->lane = (int16_t)l;
		p->tail = rnd() % 100 < 22 ? 2 + rnd() % 2 : 0;
		p->tsp = 40 + rnd() % 110;
		p->tph = rnd();
		p->wsp = 40 + rnd() % 80;
		p->wph = rnd();
		p->x0 = rnd() % XR;
		p->per = (uint8_t)(perz[p->sz] + rnd() % 3);
		age[i] = (uint8_t)(rnd() % p->per);
	}
	flow16 = flow_s = 0;
	lvs = ebase = burst = frame_n = 0;
	started = 0;
}

static inline int cy_of(int x)
{
	int s1 = sintab[((x * 728 >> 8) + p1) & (SIN_N - 1)];
	int s2 = sintab[((x * 1747 >> 8) - p2) & (SIN_N - 1)];
	return CY + ((s1 * amp1 + s2 * amp2) >> 10);
}

/* состояние частицы i в текущий момент */
static void calc(int i, st_t *s)
{
	const par_t *p = &par[i];
	int xf = (p->x0 + (int)((p->v * flow16) >> 8)) % XR;
	int x = (xf >> 4) - MARG;
	int xc = x < 0 ? 0 : x > WAVE_W - 1 ? WAVE_W - 1 : x;
	int c = cy_of(x);
	int wd = 9 + (((sintab[((x * 640 >> 8) + p3) & (SIN_N - 1)] + 1024) * 11) >> 11);
	int wob = sintab[((tq * p->wsp >> 5) + p->wph * 4) & (SIN_N - 1)] >> 10;
	int y = c + ((p->lane * wd) >> 7) + ((p->lane * burst) >> 11) + wob;
	y = y < 3 ? 3 : y > WAVE_H - 4 ? WAVE_H - 4 : y;
	int tw = sintab[((tq * p->tsp >> 4) + p->tph * 4) & (SIN_N - 1)];
	int a = p->b0 * (176 + (tw >> 4)) >> 8;
	a = (a * (190 + (spec8[xc * 204 >> 11] * 66 >> 8))) >> 8;
	a += burst * 96 >> 8;
	a = a > 255 ? 255 : a;
	s->ix = (int16_t)x;
	s->iy = (int16_t)y;
	s->lv = (uint8_t)(a >> 4);
	s->tl = 0;
	s->ts = 0;
	if (p->tail) {
		int l = p->tail + (burst >> 6);
		s->tl = (uint8_t)(l > 6 ? 6 : l);
		int ts = c - cy_of(x - 4);
		s->ts = (int8_t)(ts < -8 ? -8 : ts > 8 ? 8 : ts);
	}
}

static void box_of(int i, const st_t *s, int *x1, int *y1, int *x2, int *y2)
{
	int r = rad[par[i].sz], e = 0;
	if (s->tl)
		e = (((r + s->tl) * (s->ts < 0 ? -s->ts : s->ts)) >> 2) + 1;
	*x1 = s->ix - r - s->tl;
	*x2 = s->ix + r;
	*y1 = s->iy - r - e;
	*y2 = s->iy + r + e;
}

static wave_rect_t rc[NR];
static int nrc;

static void add_box(int x1, int y1, int x2, int y2)
{
	if (x1 < 0) x1 = 0;
	if (x2 > WAVE_W - 1) x2 = WAVE_W - 1;
	if (y1 < 0) y1 = 0;
	if (y2 > WAVE_H - 1) y2 = WAVE_H - 1;
	if (x1 > x2 || y1 > y2 || nrc == NR)
		return;
	rc[nrc++] = (wave_rect_t){ x1, y1, x2, y2 };
}

static inline int area(int x1, int y1, int x2, int y2)
{
	return (x2 - x1 + 1) * (y2 - y1 + 1);
}

/* старый и новый габарит частицы: один прямоугольник, а если они далеко друг от друга — два */
static void add_pair(int ax1, int ay1, int ax2, int ay2, int bx1_, int by1_, int bx2_, int by2_)
{
	int ux1 = ax1 < bx1_ ? ax1 : bx1_, uy1 = ay1 < by1_ ? ay1 : by1_;
	int ux2 = ax2 > bx2_ ? ax2 : bx2_, uy2 = ay2 > by2_ ? ay2 : by2_;
	if (area(ux1, uy1, ux2, uy2) <= area(ax1, ay1, ax2, ay2) + area(bx1_, by1_, bx2_, by2_) + SLACK) {
		add_box(ux1, uy1, ux2, uy2);
		return;
	}
	add_box(ax1, ay1, ax2, ay2);
	add_box(bx1_, by1_, bx2_, by2_);
}

/* слить прямоугольники: пока лишняя площадь мала или их больше WAVE_MAX_DIRTY */
static void merge_rects(void)
{
	for (;;) {
		int ba = -1, bb = 0, bc = 1 << 30;
		for (int a = 0; a < nrc; a++) {
			int ar = area(rc[a].x1, rc[a].y1, rc[a].x2, rc[a].y2);
			for (int b = a + 1; b < nrc; b++) {
				int ux1 = rc[a].x1 < rc[b].x1 ? rc[a].x1 : rc[b].x1;
				int uy1 = rc[a].y1 < rc[b].y1 ? rc[a].y1 : rc[b].y1;
				int ux2 = rc[a].x2 > rc[b].x2 ? rc[a].x2 : rc[b].x2;
				int uy2 = rc[a].y2 > rc[b].y2 ? rc[a].y2 : rc[b].y2;
				int c = area(ux1, uy1, ux2, uy2) - ar - area(rc[b].x1, rc[b].y1, rc[b].x2, rc[b].y2);
				if (c < bc) { bc = c; ba = a; bb = b; }
			}
		}
		if (ba < 0 || (nrc <= WAVE_MAX_DIRTY && bc > SLACK))
			return;
		wave_rect_t *a = &rc[ba], *b = &rc[bb];
		if (b->x1 < a->x1) a->x1 = b->x1;
		if (b->y1 < a->y1) a->y1 = b->y1;
		if (b->x2 > a->x2) a->x2 = b->x2;
		if (b->y2 > a->y2) a->y2 = b->y2;
		*b = rc[--nrc];
	}
}

static inline void put(uint16_t *dst, int x, int y, int a, int rx1, int ry1, int rx2, int ry2)
{
	if (x < rx1 || x > rx2 || y < ry1 || y > ry2)
		return;
	uint16_t *q = dst + y * WAVE_W + x;
	*q = wave_lighten(*q, a);
}

/* нарисовать частицу i, обрезав по прямоугольнику */
static void draw(uint16_t *dst, int i, int rx1, int ry1, int rx2, int ry2)
{
	const st_t *s = &st[i];
	int A = s->lv * 17, sz = par[i].sz;
	const sp_t *sp = spr[sz];
	for (int k = 0; k < spn[sz]; k++)
		put(dst, s->ix + sp[k].dx, s->iy + sp[k].dy, (A * sp[k].w) >> 6, rx1, ry1, rx2, ry2);
	int r = rad[sz];
	for (int k = 1; k <= s->tl; k++)
		put(dst, s->ix - r - k, s->iy - (((r + k) * s->ts) >> 2), (A * tailw[k - 1]) >> 6, rx1, ry1, rx2, ry2);
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	/* общие величины кадра */
	tq = (uint32_t)(f->t * 256.0);
	int dtq = (int)(f->dt * 1024.0);
	dtq = dtq < 0 ? 0 : dtq > 60 ? 60 : dtq;
	int lv = (int)(f->level * 256.0f);
	lvs += (lv - lvs) >> 3;
	int e = 0;
	for (int b = 0; b < WAVE_BANDS; b++) {
		int v = (int)(f->spec[b] * 256.0f);
		spec8[b] = v < 0 ? 0 : v > 256 ? 256 : v;
		if (b < 5)
			e += spec8[b];
	}
	e /= 5;
	int k = e - ebase - 20;
	ebase += (e - ebase) >> 4;
	if (k * 3 / 2 > burst)
		burst = k * 3 / 2 > 200 ? 200 : k * 3 / 2;
	else
		burst = burst * 236 >> 8;
	amp1 = 26 + (lvs * 14 >> 8);
	amp2 = 6 + (lvs * 6 >> 8);
	p1 = (int)(tq * 4 / 9);
	p2 = (int)(tq * 4 / 5);
	p3 = (int)(tq * 4 / 13);
	flow_s += (uint32_t)dtq * (480 * (256 + (lvs >> 2) + (burst >> 1)) >> 8);
	flow16 = (flow16 + (flow_s >> 10)) % (256 * XR);
	flow_s &= 1023;

	if (f->full || !started) {
		for (int i = 0; i < NP; i++) {
			calc(i, &st[i]);
			int x1, y1, x2, y2;
			box_of(i, &st[i], &x1, &y1, &x2, &y2);
			bx1[i] = x1; by1[i] = y1; bx2[i] = x2; by2[i] = y2;
		}
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		for (int i = 0; i < NP; i++)
			draw(dst, i, 0, 0, WAVE_W - 1, WAVE_H - 1);
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		started = 1;
		frame_n++;
		return;
	}

	nrc = 0;
	int fast = burst > 50;
	for (int j = 0, i = (frame_n++ * 37) % NP; j < NP; j++, i = i + 1 == NP ? 0 : i + 1) {
		int per = fast ? par[i].per >> 1 : par[i].per;
		if (++age[i] < per)
			continue;
		st_t n;
		calc(i, &n);
		age[i] = 0;
		const st_t *o = &st[i];
		if (n.ix == o->ix && n.iy == o->iy && n.lv == o->lv && n.tl == o->tl && n.ts == o->ts)
			continue;
		if (nrc >= NR - 2) {
			age[i] = (uint8_t)per;
			continue;
		}
		st[i] = n;
		int x1, y1, x2, y2;
		box_of(i, &st[i], &x1, &y1, &x2, &y2);
		add_pair(bx1[i], by1[i], bx2[i], by2[i], x1, y1, x2, y2);
		bx1[i] = x1; by1[i] = y1; bx2[i] = x2; by2[i] = y2;
	}
	if (!nrc)
		return;
	merge_rects();

	for (int j = 0; j < nrc; j++) {
		int rx1 = rc[j].x1, ry1 = rc[j].y1, rx2 = rc[j].x2, ry2 = rc[j].y2;
		for (int y = ry1; y <= ry2; y++)
			memcpy(dst + y * WAVE_W + rx1, bg + y * WAVE_W + rx1, sizeof(uint16_t) * (rx2 - rx1 + 1));
		for (int i = 0; i < NP; i++)
			if (bx1[i] <= rx2 && bx2[i] >= rx1 && by1[i] <= ry2 && by2[i] >= ry1)
				draw(dst, i, rx1, ry1, rx2, ry2);
		wave_dirty_add(dirty, rx1, ry1, rx2, ry2);
	}
}

const wave_t wave_dust = { "Звёздная пыль", init, render };
