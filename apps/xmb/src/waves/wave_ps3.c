/* Волна PS3: «пучок» тонких светлых линий в двух слоях. Слой A (4 линии) течёт вправо, слой B (3 линии) —
 * влево и чаще вьётся. Форма — две синусоиды по таблице (целые 8.8), фазы слегка плывут, линии пересекаются.
 * Спектр столбца растягивает пучок от центральной линии (с запасом: край пучка не выходит из полосы) и
 * делает ярче; в тишине — ровное течение. Сглаживание — ядро по дробной части y и наклону линии
 * (без ступенек на крутых местах, яркость вдоль линии ровная). Слои обновляются через кадр: у слоя
 * линии близки друг к другу, поэтому один набор прямоугольников dirty накрывает слой целиком;
 * сумма вкладов линий по пикселям лежит в acc, пересечения не портятся. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define NT 7                    /* линий всего: слой A — первые NA, остальные — слой B */
#define NA 4
#define BLK 4                   /* гранулярность границ прямоугольников, столбцов */
#define NBLK (WAVE_W / BLK)
#define NSC 12                  /* классов наклона: |наклон| = класс / 5 */
#define NFR 16                  /* шагов дробной части y */
#define CY (WAVE_H / 2)         /* центральная линия */
#define UMIN (4 * 256)
#define UMAX (148 * 256)
#define EDGE 4.5                /* запас до края полосы: ядро + округление, пикселей */

typedef struct {
	int8_t k0;                  /* первая строка относительно floor(y - 0.5) */
	uint8_t n;                  /* строк в ядре */
	uint8_t a[6];
} ker_t;

typedef struct {
	double off, amp, cyc, v, ph, wd;
	int wc, alpha;
} thr_def_t;

/* смещение от центра (px), амплитуда (px), волн на ширину, скорость (px/с), фаза (рад), частота качания фазы (Гц),
 * толщина (0 тоньше, 1 мягче), яркость */
static const thr_def_t D[NT] = {
	{ -6, 22, 1, 17, 0.00,  0.060, 0, 150 },
	{ -3, 21, 1, 17, 0.18, -0.050, 1, 130 },
	{  0, 20, 1, 17, 0.36,  0.070, 0, 120 },
	{  3, 19, 1, 17, 0.54, -0.045, 0, 100 },
	{ -2, 13, 2, -12, 1.30,  0.080, 0, 120 },
	{  2, 12, 2, -12, 1.52, -0.070, 0, 100 },
	{  6, 11, 2, -12, 1.74,  0.090, 0, 80 },
};

static ker_t ker[2][NSC][NFR];
static int16_t sn[2048];                /* sin, ±16384 */
static int fade[WAVE_W];                /* затухание к краям, 0..256 */
static int cb[WAVE_W], cf[WAVE_W];      /* столбец -> полоса спектра и дробь до следующей */
static int gx[WAVE_W], mx[WAVE_W];      /* растяжение (8.8) и яркость столбца (8.8, с краевым затуханием) */
static int gmax;                        /* предел растяжения, 8.8 */
static uint16_t acc[WAVE_W * WAVE_H];   /* сумма яркостей линий в пикселе — как у wave_lighten (0..256) */
static int16_t rb[NT][WAVE_W];          /* первая строка линии в столбце */
static uint8_t rn[NT][WAVE_W];
static uint8_t ta[NT][WAVE_W][6];
static int16_t nrb[WAVE_W];
static uint8_t nrn[WAVE_W];
static uint8_t nta[WAVE_W][6];
static int uu[WAVE_W + 2];
static int blo[NBLK], bhi[NBLK];
static int ready, fc;

static void init(void)
{
	for (int i = 0; i < 2048; i++)
		sn[i] = (int16_t)lround(16384 * sin(2 * M_PI * (i + 0.5) / 2048));
	static const double sig[2] = { 0.55, 0.80 };
	for (int w = 0; w < 2; w++)
		for (int sc = 0; sc < NSC; sc++) {
			double s = sc / 5.0, c = 1 / sqrt(1 + s * s);
			for (int fr = 0; fr < NFR; fr++) {
				double d0 = (fr + 0.5) / NFR, v[8];
				int first = -1, last = -1;
				for (int k = -3; k <= 4; k++) {     /* среднее по ширине столбца: линия наклонена */
					double m = 0;
					for (int j = 0; j < 9; j++) {
						double d = (k - d0 - s * (j - 4) / 8.0) * c;
						m += exp(-d * d / (2 * sig[w] * sig[w]));
					}
					v[k + 3] = m / 9;
					if (v[k + 3] >= 0.04) {
						if (first < 0)
							first = k;
						last = k;
					}
				}
				ker_t *e = &ker[w][sc][fr];
				if (last - first > 5)
					last = first + 5;
				e->k0 = (int8_t)first;
				e->n = (uint8_t)(last - first + 1);
				for (int i = 0; i < e->n; i++)
					e->a[i] = (uint8_t)lround(255 * v[first + i + 3]);
			}
		}
	for (int x = 0; x < WAVE_W; x++) {
		fade[x] = (int)lround(256 * (0.62 + 0.38 * sin(M_PI * (x + 0.5) / WAVE_W)));
		double p = (x - 4.5) / 10;      /* центры полос — в серединах десяток столбцов */
		int b = (int)floor(p);
		cb[x] = b;
		cf[x] = (int)lround((p - b) * 256);
	}
	double ext = 0;                     /* самый дальний от центра край пучка при растяжении 1 */
	for (int t = 0; t < NT; t++) {
		double e = fabs(D[t].off) + D[t].amp * 1.3 * 1.08;
		ext = e > ext ? e : ext;
	}
	gmax = (int)(256 * (CY - EDGE) / ext);
	if (gmax > 256 * 2)
		gmax = 256 * 2;
	ready = 0;
	fc = 0;
}

static inline uint32_t turns32(double x)
{
	x -= floor(x);
	return (uint32_t)(uint64_t)(x * 4294967296.0);
}

/* строки линии t по столбцам: первая строка, число строк, яркости */
static void thr_calc(int t, double tt, int16_t *orb, uint8_t *orn, uint8_t (*ota)[6])
{
	const thr_def_t *h = &D[t];
	double sh = -h->v * tt / WAVE_W, pd = h->ph / (2 * M_PI) + 0.04 * sin(2 * M_PI * h->wd * tt);
	uint32_t ph1 = turns32(h->cyc * sh + pd), ph2 = turns32((2 * h->cyc + 1) * sh - 1.7 * pd + 0.04 * t);
	uint32_t dk1 = (uint32_t)(h->cyc * 4294967296.0 / WAVE_W), dk2 = (uint32_t)((2 * h->cyc + 1) * 4294967296.0 / WAVE_W);
	int off = (int)(h->off * 256), A1 = (int)(h->amp * 256 * (1 + 0.08 * sin(0.45 * tt + t * 1.7)));
	int A2 = (A1 * 77) >> 8;
	for (int x = 0; x < WAVE_W; x++) {
		int d = off + ((A1 * sn[ph1 >> 21]) >> 14) + ((A2 * sn[ph2 >> 21]) >> 14);
		ph1 += dk1;
		ph2 += dk2;
		int u = CY * 256 + ((d * gx[x]) >> 8);
		uu[x + 1] = u < UMIN ? UMIN : u > UMAX ? UMAX : u;
	}
	uu[0] = uu[1];
	uu[WAVE_W + 1] = uu[WAVE_W];
	const ker_t(*kw)[NFR] = ker[h->wc];
	for (int x = 0; x < WAVE_W; x++) {
		int ds = uu[x + 2] - uu[x];
		ds = ds < 0 ? -ds : ds;
		int sc = (ds * 5 + 256) >> 9;
		sc = sc > NSC - 1 ? NSC - 1 : sc;
		int tp = uu[x + 1] - 128, r0 = tp >> 8;
		const ker_t *e = &kw[sc][(tp & 255) >> 4];
		int a = (h->alpha * mx[x]) >> 8;
		a = a > 255 ? 255 : a;
		orb[x] = (int16_t)(r0 + e->k0);
		orn[x] = e->n;
		for (int i = 0; i < e->n; i++)
			ota[x][i] = (uint8_t)((a * e->a[i]) >> 8);
	}
}

static inline uint16_t lit(uint16_t p, int a)
{
	if (a <= 0)
		return p;
	if (a > 256)
		a = 256;
	int r = p >> 11, g = (p >> 5) & 63, b = p & 31;
	r += ((31 - r) * a) >> 8;
	g += ((63 - g) * a) >> 8;
	b += ((31 - b) * a) >> 8;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

/* прямоугольники по блокам столбцов: минимальная высота hl, при которой укладываемся в m штук */
static int segs(int hl, int m, wave_dirty_t *d)
{
	int n = 0, s = 0, mn = blo[0], mx_ = bhi[0];
	for (int b = 1; b <= NBLK; b++) {
		int nmn = mn, nmx = mx_;
		if (b < NBLK) {
			nmn = blo[b] < mn ? blo[b] : mn;
			nmx = bhi[b] > mx_ ? bhi[b] : mx_;
		}
		if (b == NBLK || nmx - nmn + 1 > hl) {
			if (mx_ >= mn) {
				if (d)
					wave_dirty_add(d, s * BLK, mn, b * BLK - 1, mx_);
				n++;
				if (n > m && !d)
					return n;
			}
			s = b;
			if (b < NBLK) {
				mn = blo[b];
				mx_ = bhi[b];
			}
		} else {
			mn = nmn;
			mx_ = nmx;
		}
	}
	return n;
}

static void emit(wave_dirty_t *d, int m)
{
	int lo = 3, hi = WAVE_H;
	while (lo < hi) {
		int mid = (lo + hi) >> 1;
		if (segs(mid, m, NULL) <= m)
			hi = mid;
		else
			lo = mid + 1;
	}
	segs(lo, m, d);
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	/* спектр -> столбцы (сглаженный между полосами) */
	int s[WAVE_BANDS + 2], sm[WAVE_BANDS + 2];
	for (int b = 0; b < WAVE_BANDS; b++) {
		int v = f->spec ? (int)(f->spec[b] * 256) : 0;
		s[b + 1] = v < 0 ? 0 : v > 256 ? 256 : v;
	}
	s[0] = s[1];
	s[WAVE_BANDS + 1] = s[WAVE_BANDS];
	for (int b = 0; b < WAVE_BANDS + 2; b++)
		sm[b] = b == 0 || b == WAVE_BANDS + 1 ? s[b] : (s[b - 1] + 2 * s[b] + s[b + 1]) >> 2;
	int lvl = (int)(f->level * 256);
	lvl = lvl < 0 ? 0 : lvl > 256 ? 256 : lvl;
	for (int x = 0; x < WAVE_W; x++) {      /* cb может быть -1: это s[0] */
		int b = cb[x] + 1;
		int v = sm[b] + (((sm[b + 1] - sm[b]) * cf[x]) >> 8);
		int e = (v * (102400 - 144 * v)) >> 16;
		e += (lvl * 40) >> 8;
		e = e > 256 ? 256 : e;
		gx[x] = 256 + (((gmax - 256) * e * 160) >> 16);   /* пучок раскрывается умеренно: поверх — текст меню */
		int m = 135 + ((v * 70) >> 8) + ((lvl * 20) >> 8);
		mx[x] = (m * fade[x]) >> 8;
	}

	if (f->full || !ready) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		memset(acc, 0, sizeof(acc));
		for (int t = 0; t < NT; t++) {
			thr_calc(t, f->t, rb[t], rn[t], ta[t]);
			for (int x = 0; x < WAVE_W; x++)
				for (int k = 0; k < rn[t][x]; k++)
					acc[(rb[t][x] + k) * WAVE_W + x] += ta[t][x][k];
		}
		for (int t = 0; t < NT; t++)
			for (int x = 0; x < WAVE_W; x++)
				for (int k = 0; k < rn[t][x]; k++) {
					int i = (rb[t][x] + k) * WAVE_W + x;
					dst[i] = lit(bg[i], acc[i]);
				}
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		ready = 1;
		fc = 0;
		return;
	}

	fc++;
	int t0 = fc & 1 ? 0 : NA, t1 = fc & 1 ? NA : NT;
	for (int b = 0; b < NBLK; b++) {
		blo[b] = WAVE_H;
		bhi[b] = -1;
	}
	for (int t = t0; t < t1; t++) {
		thr_calc(t, f->t, nrb, nrn, nta);
		for (int x = 0; x < WAVE_W; x++) {
			int ro = rb[t][x], no = rn[t][x], rnw = nrb[x], nn = nrn[x];
			if (ro == rnw && no == nn && !memcmp(ta[t][x], nta[x], 6))
				continue;
			uint16_t *p = acc + ro * WAVE_W + x;
			for (int k = 0; k < no; k++, p += WAVE_W)
				*p -= ta[t][x][k];
			p = acc + rnw * WAVE_W + x;
			for (int k = 0; k < nn; k++, p += WAVE_W)
				*p += nta[x][k];
			memcpy(ta[t][x], nta[x], 6);
			rb[t][x] = (int16_t)rnw;
			rn[t][x] = (uint8_t)nn;
			int a = ro < rnw ? ro : rnw, b = (ro + no > rnw + nn ? ro + no : rnw + nn) - 1;
			for (int y = a; y <= b; y++) {
				int i = y * WAVE_W + x;
				dst[i] = lit(bg[i], acc[i]);
			}
			int bi = x / BLK;
			blo[bi] = a < blo[bi] ? a : blo[bi];
			bhi[bi] = b > bhi[bi] ? b : bhi[bi];
		}
	}
	emit(dirty, WAVE_MAX_DIRTY);
}

const wave_t wave_ps3 = { "Волна PS3", init, render };
