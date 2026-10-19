/* Круги на воде: тонкие сглаженные эллипсы (вид в перспективе) расходятся от случайных точек и тают.
 * Музыка (wave_music.c): каждая бочка — свой круг с внутренним следом, размер и яркость — по силе удара,
 * в первые доли секунды он быстро раскрывается; малый барабан — круг поменьше, одинарный и короче; хэт —
 * мелкая капля. Сглаженный бас — дыхание: все круги чуть ярче; когда играет плотно, круги расходятся быстрее.
 * На каждый вид — свои слоты: при частых ударах место уступает самый старый (уже бледный) круг того же вида,
 * поэтому кругов на воде не больше MAXR. Без бочки (и в тишине) — редкие тихие капли.
 *
 * Кадр: кольца, что изменились, вместе с теми, что их касаются (замыкание по габаритам), стираются
 * по старому положению (след восстанавливается из bg) и рисуются заново в одном порядке слотов —
 * общий участок всегда собран одинаково. dirty — только по изменившимся кольцам, дугами-сегментами:
 * ограничивающий прямоугольник сегмента по старому и новому положению. Всё в целых числах 8.8. */
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define NBIG 4                  /* слоты: бочка и тихие капли, малый барабан, хэт */
#define NSN 2
#define NHAT 2
#define MAXR (NBIG + NSN + NHAT)
#define ASPECT 88               /* ry = rx * 88/256 */
#define XS_K 242                /* cos θ0 * 256: до него эллипс «пологий» (идём по столбцам) */
#define YS_K 83                 /* sin θ0 * 256: дальше — по строкам */
#define SQN 1024
#define MAX_SLICE 12
#define MARGIN 2                /* запас под ядро сглаживания */
#define BR_MIN 10               /* кольцо бледнее — считаем растаявшим */
#define STEP_R 340              /* порог смещения радиуса, 8.8 */
#define STEP_B 16               /* порог смены яркости */
#define RMIN8 (3 * 256)
#define INNER_K 200             /* радиус внутреннего кольца, /256 */
#define INNER_B 120             /* его яркость, /256 */
#define INNER_MIN (18 * 256)

typedef struct {
	int cx, cy, rx8, ry8, br, in;   /* in — с внутренним кольцом */
} geo_t;

typedef struct {
	int on, in;
	double ph, inv_life;    /* возраст в долях жизни */
	const uint16_t *rp;     /* рост радиуса */
	int rmax, amp, cx, cy;
	int dvalid;             /* кольцо сейчас нарисовано в dst по геометрии d */
	geo_t d;
} ring_t;

static uint16_t SQ[SQN + 1];            /* sqrt(1-u²), u = i/1024, 0..65535 */
static uint8_t KW[16][4];               /* веса четырёх строк ядра по дробной части */
static uint16_t RP[257];                /* рост радиуса по возрасту, 0..65535 */
static uint16_t RP3[257];               /* он же для удара: быстрое раскрытие */
static uint16_t FP[257];                /* яркость по возрасту, 0..256 */
static int16_t SLT[MAX_SLICE + 1][MAX_SLICE][4];   /* габариты дуги в долях радиуса /256: x1 x2 y1 y2 */

static ring_t rg[MAXR];
static uint32_t rnd_s = 12345;
static double next_drop;
static unsigned last_k, last_s, last_h;
static int started, last_x;

static uint32_t rnd(void)
{
	rnd_s ^= rnd_s << 13;
	rnd_s ^= rnd_s >> 17;
	rnd_s ^= rnd_s << 5;
	return rnd_s;
}

static void init(void)
{
	for (int i = 0; i <= SQN; i++) {
		double u = (double)i / SQN;
		SQ[i] = (uint16_t)(65535.0 * sqrt(1 - u * u) + 0.5);
	}
	for (int q = 0; q < 16; q++)
		for (int k = 0; k < 4; k++) {
			double d = fabs((k - 1) - (q + 0.5) / 16), w = 1 - d / 1.5;
			KW[q][k] = (uint8_t)(w > 0 ? w * 255 : 0);
		}
	for (int i = 0; i <= 256; i++) {
		double p = i / 256.0;
		RP[i] = (uint16_t)(65535.0 * (1 - (1 - p) * (1 - p)));
		RP3[i] = (uint16_t)(65535.0 * (1 - (1 - p) * (1 - p) * (1 - p)));
		double fi = p < 0.05 ? p / 0.05 : 1;
		FP[i] = (uint16_t)(256.0 * pow(1 - p, 1.3) * fi * fi);
	}
	for (int n = 1; n <= MAX_SLICE; n++)
		for (int s = 0; s < n; s++) {
			double a0 = 2 * M_PI * s / n, a1 = 2 * M_PI * (s + 1) / n;
			double x1 = 9, x2 = -9, y1 = 9, y2 = -9;
			for (int k = 0; k <= 90; k++) {
				double a = a0 + (a1 - a0) * k / 90, c = cos(a), sn = sin(a);
				x1 = c < x1 ? c : x1;
				x2 = c > x2 ? c : x2;
				y1 = sn < y1 ? sn : y1;
				y2 = sn > y2 ? sn : y2;
			}
			SLT[n][s][0] = (int16_t)floor(x1 * 256 - 1);
			SLT[n][s][1] = (int16_t)ceil(x2 * 256 + 1);
			SLT[n][s][2] = (int16_t)floor(y1 * 256 - 1);
			SLT[n][s][3] = (int16_t)ceil(y2 * 256 + 1);
		}
	memset(rg, 0, sizeof(rg));
	rnd_s = 12345;
	started = 0;
	last_x = WAVE_W / 2;
}

/* новое кольцо в слотах [i0, i1): свободный, иначе самый старый */
static void spawn(int i0, int i1, int amp, int rmax, double life, const uint16_t *rp, int in)
{
	int k = i0;
	for (int i = i0; i < i1; i++) {
		if (!rg[i].on) {
			k = i;
			break;
		}
		if (rg[i].ph > rg[k].ph)
			k = i;
	}
	ring_t *r = &rg[k];
	int x;
	do
		x = 24 + (int)(rnd() % (WAVE_W - 48));
	while (abs(x - last_x) < 70);
	last_x = x;
	r->on = 1;
	r->in = in;
	r->ph = 0;
	r->inv_life = 1 / life;
	r->rp = rp;
	r->rmax = rmax;
	r->amp = amp;
	r->cx = x;
	r->cy = 40 + (int)(rnd() % (WAVE_H - 80));
}

/* нарисовать (или стереть до bg) кольцо: пологая часть — по столбцам, крутые концы — по строкам;
 * вдоль нормали — ядро из 4 пикселей по дробной части позиции */
static void trace1(uint16_t *dst, const uint16_t *bg, const geo_t *g, int erase)
{
	int rx8 = g->rx8, ry8 = g->ry8, cx = g->cx, cy = g->cy, br = g->br;
	int xs = (rx8 * XS_K) >> 16, ys = (ry8 * YS_K) >> 16;
	int inv = (1 << 26) / rx8, d0 = -xs, d1 = xs;
	if (cx + d0 < 0)
		d0 = -cx;
	if (cx + d1 > WAVE_W - 1)
		d1 = WAVE_W - 1 - cx;
	for (int dx = d0; dx <= d1; dx++) {
		int ad = dx < 0 ? -dx : dx;
		int yf = (ry8 * SQ[(ad * inv) >> 8]) >> 16;
		for (int s = 0; s < 2; s++) {
			int y8 = cy * 256 + (s ? yf : -yf);
			int yp = (y8 >> 8) - 1;
			const uint8_t *w = KW[(y8 & 255) >> 4];
			uint16_t *pd = dst + yp * WAVE_W + cx + dx;
			const uint16_t *pb = bg + yp * WAVE_W + cx + dx;
			for (int k = 0; k < 4; k++, pd += WAVE_W, pb += WAVE_W) {
				if ((unsigned)(yp + k) >= WAVE_H)
					continue;
				if (erase)
					*pd = *pb;
				else
					*pd = wave_lighten(*pd, (w[k] * br) >> 8);
			}
		}
	}
	int invy = (1 << 26) / ry8, e0 = -ys, e1 = ys;
	if (cy + e0 < 0)
		e0 = -cy;
	if (cy + e1 > WAVE_H - 1)
		e1 = WAVE_H - 1 - cy;
	for (int dy = e0; dy <= e1; dy++) {
		int ad = dy < 0 ? -dy : dy;
		int xf = (int)(((unsigned)rx8 * SQ[(ad * invy) >> 8]) >> 16);
		int row = (cy + dy) * WAVE_W;
		for (int s = 0; s < 2; s++) {
			int x8 = cx * 256 + (s ? xf : -xf);
			int xp = (x8 >> 8) - 1;
			const uint8_t *w = KW[(x8 & 255) >> 4];
			for (int k = 0; k < 4; k++) {
				int x = xp + k;
				if ((unsigned)x >= WAVE_W)
					continue;
				if (erase)
					dst[row + x] = bg[row + x];
				else
					dst[row + x] = wave_lighten(dst[row + x], (w[k] * br) >> 8);
			}
		}
	}
}

/* внутреннее кольцо — тусклый след за основным */
static void inner(const geo_t *g, geo_t *o)
{
	*o = (geo_t){ g->cx, g->cy, (g->rx8 * INNER_K) >> 8, (g->ry8 * INNER_K) >> 8, (g->br * INNER_B) >> 8, 0 };
}

static void trace(uint16_t *dst, const uint16_t *bg, const geo_t *g, int erase)
{
	trace1(dst, bg, g, erase);
	if (g->in && g->rx8 >= INNER_MIN) {
		geo_t o;
		inner(g, &o);
		trace1(dst, bg, &o, erase);
	}
}

/* габарит кольца целиком (с запасом) */
static void bbox(const geo_t *g, int *b)
{
	b[0] = g->cx - (g->rx8 >> 8) - 1 - MARGIN;
	b[1] = g->cx + (g->rx8 >> 8) + 1 + MARGIN;
	b[2] = g->cy - (g->ry8 >> 8) - 1 - MARGIN;
	b[3] = g->cy + (g->ry8 >> 8) + 1 + MARGIN;
}

static int hit(const int *a, const int *b)
{
	return a[0] <= b[1] && b[0] <= a[1] && a[2] <= b[3] && b[2] <= a[3];
}

static void add_arcs(wave_dirty_t *d, const geo_t *a, const geo_t *b, int n)
{
	geo_t gs[4];
	int ng = 0;
	for (int k = 0; k < 2; k++) {
		const geo_t *g = k ? b : a;
		if (!g)
			continue;
		gs[ng++] = *g;
		if (g->in && g->rx8 >= INNER_MIN)
			inner(g, &gs[ng++]);
	}
	for (int s = 0; s < n; s++) {
		const int16_t *sl = SLT[n][s];
		int x1 = 9999, x2 = -9999, y1 = 9999, y2 = -9999;
		for (int k = 0; k < ng; k++) {
			const geo_t *g = &gs[k];
			int v;
			v = g->cx + ((g->rx8 * sl[0]) >> 16) - MARGIN;
			x1 = v < x1 ? v : x1;
			v = g->cx + ((g->rx8 * sl[1] + 65535) >> 16) + MARGIN;
			x2 = v > x2 ? v : x2;
			v = g->cy + ((g->ry8 * sl[2]) >> 16) - MARGIN;
			y1 = v < y1 ? v : y1;
			v = g->cy + ((g->ry8 * sl[3] + 65535) >> 16) + MARGIN;
			y2 = v > y2 ? v : y2;
		}
		wave_dirty_add(d, x1, y1, x2, y2);
	}
}

static const int NSTEP[] = { 1, 2, 4, 6, 8, 12 };

static int want_n(int rpx)
{
	return rpx < 12 ? 0 : rpx < 26 ? 1 : rpx < 42 ? 2 : rpx < 60 ? 3 : rpx < 84 ? 4 : 5;
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	static const wave_music_t calm;
	const wave_music_t *m = f->m ? f->m : &calm;
	double t = f->t, dt = f->dt > 0.1 ? 0.1 : f->dt;
	/* рождение колец */
	if (!started) {
		next_drop = t + 0.5;
		last_k = m->kicks;
		last_s = m->snares;
		last_h = m->hats;
		started = 1;
	}
	if (m->kicks != last_k) {
		last_k = m->kicks;
		float s = m->kick;
		int rmax = 52 + (int)(rnd() % 20) + (int)(45 * s);
		spawn(0, NBIG, 140 + (int)(110 * s), rmax, 2.0 + (rnd() % 100) / 200.0, RP3, 1);
		next_drop = t + 2 + (rnd() % 100) / 50.0;
	}
	if (m->snares != last_s) {
		last_s = m->snares;
		float s = m->snare;
		spawn(NBIG, NBIG + NSN, 110 + (int)(100 * s), 30 + (int)(rnd() % 10) + (int)(20 * s), 1.3, RP3, 0);
	}
	if (m->hats != last_h) {
		last_h = m->hats;
		spawn(NBIG + NSN, MAXR, 70 + (int)(90 * m->hat), 9 + (int)(rnd() % 6), 0.7, RP3, 0);
	}
	if (t >= next_drop) {
		spawn(0, NBIG, 110 + (int)(rnd() % 40), 50 + (int)(rnd() % 35), 3.6 + (rnd() % 100) / 120.0, RP, 1);
		next_drop = t + 2 + (rnd() % 100) / 50.0;
	}
	double rate = dt * (1 + 0.6 * m->energy);
	int lift = 256 + (int)(m->bass_att * 110);

	/* новая геометрия колец; без изменений — остаётся прежняя */
	geo_t ng[MAXR];
	int nv[MAXR], ch[MAXR];
	for (int i = 0; i < MAXR; i++) {
		ring_t *r = &rg[i];
		nv[i] = 0;
		if (r->on) {
			r->ph += rate * r->inv_life;
			int p = (int)(r->ph * 256);
			if (p >= 256) {
				r->on = 0;
			} else {
				int br = (((r->amp * lift) >> 8) * FP[p]) >> 8;
				br = br > 256 ? 256 : br;
				if (br < BR_MIN && p > 128) {
					r->on = 0;
				} else {
					int rx8 = (r->rmax * r->rp[p]) >> 8;
					rx8 = rx8 < RMIN8 ? RMIN8 : rx8;
					ng[i] = (geo_t){ r->cx, r->cy, rx8, (rx8 * ASPECT) >> 8, br, r->in };
					nv[i] = 1;
				}
			}
		}
		if (nv[i] && !r->dvalid)
			ch[i] = 1;
		else if (!nv[i])
			ch[i] = r->dvalid;
		else {
			const geo_t *o = &r->d;
			ch[i] = o->cx != ng[i].cx || o->cy != ng[i].cy || o->in != ng[i].in ||
				abs(o->rx8 - ng[i].rx8) >= STEP_R || abs(o->br - ng[i].br) >= STEP_B;
			if (!ch[i])
				ng[i] = *o;
		}
	}

	if (f->full || !started) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		for (int i = 0; i < MAXR; i++) {
			rg[i].dvalid = nv[i];
			if (nv[i]) {
				rg[i].d = ng[i];
				trace(dst, bg, &ng[i], 0);
			}
		}
		return;
	}

	int any = 0;
	for (int i = 0; i < MAXR; i++)
		any |= ch[i];
	if (!any)
		return;

	/* габариты (старые ∪ новые) и замыкание: кольцо, касающееся изменившегося, перерисовывается тоже */
	int box[MAXR][4], aff[MAXR];
	for (int i = 0; i < MAXR; i++) {
		aff[i] = ch[i];
		int has = 0, b[4];
		if (rg[i].dvalid) {
			bbox(&rg[i].d, box[i]);
			has = 1;
		}
		if (nv[i]) {
			bbox(&ng[i], b);
			if (!has)
				memcpy(box[i], b, sizeof(b));
			else {
				box[i][0] = b[0] < box[i][0] ? b[0] : box[i][0];
				box[i][1] = b[1] > box[i][1] ? b[1] : box[i][1];
				box[i][2] = b[2] < box[i][2] ? b[2] : box[i][2];
				box[i][3] = b[3] > box[i][3] ? b[3] : box[i][3];
			}
			has = 1;
		}
		if (!has)
			aff[i] = -1;    /* кольца нет совсем */
	}
	for (int again = 1; again;) {
		again = 0;
		for (int i = 0; i < MAXR; i++) {
			if (aff[i])
				continue;
			for (int j = 0; j < MAXR; j++)
				if (aff[j] == 1 && hit(box[i], box[j])) {
					aff[i] = 1;
					again = 1;
					break;
				}
		}
	}

	/* dirty по изменившимся кольцам; число дуг подгоняем под лимит */
	int ni[MAXR], sum = 0;
	for (int i = 0; i < MAXR; i++) {
		ni[i] = 0;
		if (!ch[i])
			continue;
		int rpx = ((nv[i] ? ng[i].rx8 : rg[i].d.rx8) >> 8);
		if (nv[i] && rg[i].dvalid && rg[i].d.rx8 > ng[i].rx8)
			rpx = rg[i].d.rx8 >> 8;
		ni[i] = want_n(rpx);
		sum += NSTEP[ni[i]];
	}
	while (sum > WAVE_MAX_DIRTY) {
		int m = 0;
		for (int i = 1; i < MAXR; i++)
			if (ni[i] > ni[m])
				m = i;
		sum -= NSTEP[ni[m]] - NSTEP[ni[m] - 1];
		ni[m]--;
	}
	for (int i = 0; i < MAXR; i++)
		if (ch[i])
			add_arcs(dirty, rg[i].dvalid ? &rg[i].d : NULL, nv[i] ? &ng[i] : NULL, NSTEP[ni[i]]);

	/* стереть всё затронутое по старому положению, затем нарисовать по новому — в порядке слотов */
	for (int i = 0; i < MAXR; i++)
		if (aff[i] == 1 && rg[i].dvalid)
			trace(dst, bg, &rg[i].d, 1);
	for (int i = 0; i < MAXR; i++)
		if (aff[i] == 1) {
			rg[i].dvalid = nv[i];
			if (nv[i]) {
				rg[i].d = ng[i];
				trace(dst, bg, &ng[i], 0);
			}
		}
}

const wave_t wave_ripple = { "Круги", init, render };
