/* Шёлк: несколько тонких сглаженных нитей-лент. Положение — две синусоиды и медленный дрейф по таблице
 * синуса (целые числа 8.8), на краях нити тускнеют. Музыка (wave_music.c): нити дышат на сглаженном басу —
 * волны глубже; каждая бочка пускает по полосе мягкую волну — нити в этом месте расходятся от середины
 * полосы и светлеют, и место бежит слева направо; течение быстрее, когда играет плотно, и с рывком на
 * бочке (время течения копится — без рывков назад); малый барабан на миг разводит нити веером; хэт пускает
 * по одной из нитей скользящий блик; спектр столбца — лёгкий подъём и подсветка. Разведение мягко
 * ограничено — нити не упираются в края полосы. В тишине — ровное течение. Нити обновляются по очереди
 * (раз в NPH кадров) — вдвое меньше работы и перерисовки; сумма вкладов нитей по пикселям лежит в acc,
 * поэтому пересечения не портят друг друга. dirty — прямоугольники по столбцам переменной ширины (жадно,
 * под бюджет прямоугольников), на каждую нить. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define NT 5                    /* нитей */
#define NPH 2                   /* нить обновляется раз в NPH кадров */
#define BLK 4                   /* гранулярность границ прямоугольников, столбцов */
#define NBLK (WAVE_W / BLK)
#define UMAX ((WAVE_H - 3) * 256 + 255)
#define CY (WAVE_H / 2 * 256)   /* середина полосы, 8.8 */
#define KNEE (48 * 256)         /* дальше от середины разведение сжимается... */
#define KR (14 * 256)           /* ...и не уходит дальше KNEE + KR */
#define NG 3                    /* бликов хэта */
#define GR 30                   /* полуширина блика, столбцов */
#define PR 20                   /* радиус сглаживания импульса бочки, столбцов */

typedef struct {
	int c, a1, a2, d;           /* центр, амплитуды, амплитуда дрейфа — пиксели 8.8 */
	uint32_t dk1, dk2;          /* приращение фазы на столбец */
	double p1, w1, p2, w2, pd, wd;   /* фаза и скорость в оборотах (и об/с) */
	int alpha, lift, sheen;     /* яркость, подъём спектром (пикс 8.8), блик вдоль нити */
} thr_t;

/* c, a1, k1, w1, p1, a2, k2, w2, p2, d, wd, pd, alpha, lift, sheen */
static const double P[NT][15] = {
	{ 50, 15, 0.85,  0.09, 0.10, 6, 2.1, -0.13, 0.50,  8,  0.045, 0.0, 120, 6, 0.40 },
	{ 66, 17, 1.10, -0.07, 0.35, 7, 2.6,  0.11, 0.20,  9, -0.050, 0.3,  88, 5, 0.35 },
	{ 80, 14, 0.70,  0.06, 0.62, 6, 1.9, -0.10, 0.80, 10,  0.038, 0.6, 140, 7, 0.45 },
	{ 94, 16, 1.25, -0.10, 0.80, 8, 3.0,  0.14, 0.10,  8, -0.060, 0.1, 100, 5, 0.35 },
	{ 108, 14, 0.95,  0.08, 0.05, 6, 2.4, -0.09, 0.45,  7,  0.050, 0.8, 110, 6, 0.40 },
};

static thr_t th[NT];
static int16_t sn[1025];                /* sin, ±16384 */
static uint8_t ker[16][4];              /* профиль нити по 4 строкам для дроби y (шаг 1/16 пикселя) */
static int fade[WAVE_W];                /* затухание к краям, 0..256 */
static int cb[WAVE_W], cf[WAVE_W];      /* столбец -> полоса спектра и дробь до следующей */
static int cs[WAVE_W];                  /* спектр по столбцам, 0..256 (на кадр) */
static uint16_t acc[WAVE_W * WAVE_H];   /* сумма яркостей нитей в пикселе, 0..256 — как у wave_lighten */
static int16_t rb[NT][WAVE_W];          /* первая из 4 строк нити в столбце */
static uint8_t ta[NT][WAVE_W][4];       /* яркости 4 строк */
static int16_t nrb[WAVE_W];
static uint8_t nta[WAVE_W][4];
static int blo[NBLK], bhi[NBLK];
static int ready, fc;
static double ft;                       /* время течения: идёт быстрее, когда играет плотно */
static wave_pulses_t pl;                /* импульсы бочки по полосе */
static int pls[WAVE_W];                 /* их сумма по столбцам, сглаженная, 0..256 */
static int gx[WAVE_W];                  /* разведение от середины по столбцам, 8.8 */
static float snf;                       /* малый с мягким фронтом */
static struct { float x, v, a; int t; } gs[NG];         /* блики хэта: столбец, скорость, сила, нить */
static uint8_t gp[2 * GR + 1];          /* профиль блика */
static unsigned ghats, rnd;

static void init(void)
{
	for (int i = 0; i <= 1024; i++)
		sn[i] = (int16_t)lround(16384 * sin(2 * M_PI * i / 1024));
	for (int j = 0; j < 16; j++) {      /* гауссиана σ≈0.65 px от центра строки до центра нити */
		double d0 = (j + 0.5) / 16, dd[4] = { 1 + d0, d0, 1 - d0, 2 - d0 };
		for (int k = 0; k < 4; k++)
			ker[j][k] = (uint8_t)lround(255 * exp(-dd[k] * dd[k] / (2 * 0.65 * 0.65)));
	}
	for (int i = 0; i <= 2 * GR; i++) {
		double u = (i - GR) / 12.0;
		gp[i] = (uint8_t)lround(255 * exp(-u * u));
	}
	for (int x = 0; x < WAVE_W; x++) {
		fade[x] = (int)lround(256 * (0.12 + 0.88 * sin(M_PI * (x + 0.5) / WAVE_W)));
		double p = (x - 4.5) / 10;      /* центры полос — в серединах десяток столбцов */
		int b = (int)floor(p);
		cb[x] = b;
		cf[x] = (int)lround((p - b) * 256);
	}
	for (int t = 0; t < NT; t++) {
		const double *p = P[t];
		th[t] = (thr_t){ (int)(p[0] * 256), (int)(p[1] * 256), (int)(p[5] * 256), (int)(p[9] * 256),
				 (uint32_t)(p[2] * 4294967296.0 / WAVE_W), (uint32_t)(p[6] * 4294967296.0 / WAVE_W),
				 p[4], p[3], p[8], p[7], p[11], p[10], (int)p[12], (int)(p[13] * 256), (int)(p[14] * 256) };
	}
	memset(&pl, 0, sizeof(pl));
	memset(gs, 0, sizeof(gs));
	ready = 0;
	fc = 0;
	ft = 0;
	snf = 0;
}

static inline uint32_t turns32(double x)
{
	x -= floor(x);
	return (uint32_t)(uint64_t)(x * 4294967296.0);
}

static inline int ssin(uint32_t ph)
{
	unsigned i = ph >> 22, fr = (ph >> 16) & 63;
	int a = sn[i];
	return a + (((sn[i + 1] - a) * (int)fr) >> 6);
}

/* положение и яркости нити t по столбцам; mulA — размах волн, lvB — прибавка яркости, g — блики (0..256) */
static void thr_calc(int t, double tt, int mulA, int lvB, const int *g, int16_t *orb, uint8_t (*ota)[4])
{
	const thr_t *h = &th[t];
	uint32_t ph1 = turns32(h->p1 + h->w1 * tt), ph2 = turns32(h->p2 + h->w2 * tt);
	int base = h->c + (int)(h->d * sin(2 * M_PI * (h->pd + h->wd * tt))) - CY;
	int A1 = (h->a1 * mulA) >> 8, A2 = (h->a2 * mulA) >> 8;
	for (int x = 0; x < WAVE_W; x++) {
		int s1 = ssin(ph1), s2 = ssin(ph2);
		ph1 += h->dk1;
		ph2 += h->dk2;
		int d = base + ((A1 * s1) >> 14) + ((A2 * s2) >> 14);
		d = (d * gx[x]) >> 8;
		int ad = d < 0 ? -d : d;
		if (ad > KNEE) {
			int e = ad - KNEE;
			ad = KNEE + (int)((int64_t)e * KR / (e + KR));
			d = d < 0 ? -ad : ad;
		}
		int u = CY + d - ((cs[x] * h->lift) >> 8) - 128;
		u = u < 256 ? 256 : u > UMAX ? UMAX : u;
		int m = 256 + lvB + ((cs[x] * 90) >> 8) + ((s2 * h->sheen) >> 14) + ((pls[x] * 200) >> 8) + g[x];
		int a = (((h->alpha * fade[x]) >> 8) * m) >> 8;
		a = a > 255 ? 255 : a;
		const uint8_t *kw = ker[(u & 255) >> 4];
		orb[x] = (int16_t)((u >> 8) - 1);
		ota[x][0] = (uint8_t)((a * kw[0]) >> 8);
		ota[x][1] = (uint8_t)((a * kw[1]) >> 8);
		ota[x][2] = (uint8_t)((a * kw[2]) >> 8);
		ota[x][3] = (uint8_t)((a * kw[3]) >> 8);
	}
}

/* прямоугольники по блокам столбцов: минимальная высота Hlim, при которой укладываемся в m штук */
static int segs(int hl, int m, wave_dirty_t *d)
{
	int n = 0, s = 0, mn = blo[0], mx = bhi[0];
	for (int b = 1; b <= NBLK; b++) {
		int nmn = mn, nmx = mx;
		if (b < NBLK) {
			nmn = blo[b] < mn ? blo[b] : mn;
			nmx = bhi[b] > mx ? bhi[b] : mx;
		}
		if (b == NBLK || nmx - nmn + 1 > hl) {
			if (d)
				wave_dirty_add(d, s * BLK, mn, b * BLK - 1, mx);
			n++;
			if (n > m && !d)
				return n;
			s = b;
			if (b < NBLK) {
				mn = blo[b];
				mx = bhi[b];
			}
		} else {
			mn = nmn;
			mx = nmx;
		}
	}
	return n;
}

static void emit(wave_dirty_t *d, int m)
{
	int lo = 4, hi = WAVE_H;
	while (lo < hi) {
		int mid = (lo + hi) >> 1;
		if (segs(mid, m, NULL) <= m)
			hi = mid;
		else
			lo = mid + 1;
	}
	segs(lo, m, d);
}

/* скользящее среднее по 2·PR+1 столбцам (края — повтором крайнего) */
static void smooth(int *v)
{
	int o[WAVE_W], s = 0;
	for (int i = -PR - 1; i < PR; i++)
		s += v[i < 0 ? 0 : i];
	for (int x = 0; x < WAVE_W; x++) {
		int a = x + PR, b = x - PR - 1;
		s += v[a > WAVE_W - 1 ? WAVE_W - 1 : a] - v[b < 0 ? 0 : b];
		o[x] = s * (65536 / (2 * PR + 1)) >> 16;
	}
	memcpy(v, o, sizeof(o));
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	static const wave_music_t calm;
	const wave_music_t *mu = f->m ? f->m : &calm;
	float dt = f->dt > 0.1 ? 0.1f : (float)f->dt;
	float sp = 1 + 1.0f * mu->energy + 1.8f * mu->kick;
	ft += dt * sp;
	wave_pulses_update(&pl, mu, dt, pls);
	smooth(pls);
	smooth(pls);
	snf += (mu->snare - snf) * (mu->snare > snf ? (dt >= 0.04f ? 1 : dt / 0.04f) : 1);
	int sns = (int)(snf * 256);
	for (int x = 0; x < WAVE_W; x++)
		gx[x] = 256 + ((pls[x] * 150) >> 8) + ((sns * 80) >> 8);

	/* блики хэта: по нитям по кругу, скользят по течению */
	static int g[NT][WAVE_W];
	if (mu->hats != ghats) {
		ghats = mu->hats;
		int k = 0;
		for (int i = 1; i < NG; i++)
			if (gs[i].a < gs[k].a)
				k = i;
		rnd = rnd * 1103515245u + 12345u;
		gs[k].t = (int)(ghats % NT);
		gs[k].x = (float)(50 + (rnd >> 16) % 220);
		gs[k].v = P[gs[k].t][3] > 0 ? -150 : 150;
		gs[k].a = mu->hat;
	}
	memset(g, 0, sizeof(g));
	for (int i = 0; i < NG; i++) {
		if (gs[i].a < 0.03f)
			continue;
		gs[i].x += gs[i].v * sp * dt;
		gs[i].a *= expf(-dt / 0.35f);
		int a = (int)(gs[i].a * 360), cx = (int)gs[i].x, *gt = g[gs[i].t];
		for (int x = cx - GR < 0 ? 0 : cx - GR; x < WAVE_W && x <= cx + GR; x++)
			gt[x] += (a * gp[x - cx + GR]) >> 8;
	}

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
	for (int x = 0; x < WAVE_W; x++) {      /* cb может быть -1: это s[0] */
		int b = cb[x] + 1;
		cs[x] = sm[b] + (((sm[b + 1] - sm[b]) * cf[x]) >> 8);
	}
	int bas = (int)(mu->bass_att * 256), en = (int)(mu->energy * 256);
	int mulA = 256 + ((bas * 90) >> 8), lvB = ((en * 50) >> 8) + ((sns * 70) >> 8);

	if (f->full || !ready) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		memset(acc, 0, sizeof(acc));
		for (int t = 0; t < NT; t++) {
			thr_calc(t, ft, mulA, lvB, g[t], rb[t], ta[t]);
			for (int x = 0; x < WAVE_W; x++)
				for (int k = 0; k < 4; k++)
					acc[(rb[t][x] + k) * WAVE_W + x] += ta[t][x][k];
		}
		for (int t = 0; t < NT; t++)
			for (int x = 0; x < WAVE_W; x++)
				for (int k = 0; k < 4; k++) {
					int i = (rb[t][x] + k) * WAVE_W + x;
					dst[i] = wave_lighten(bg[i], acc[i]);
				}
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		ready = 1;
		fc = 0;
		return;
	}

	fc++;
	int upd[NT], nu = 0;
	for (int t = 0; t < NT; t++)
		if (t % NPH == fc % NPH)
			upd[nu++] = t;
	for (int u = 0; u < nu; u++) {
		int t = upd[u];
		thr_calc(t, ft, mulA, lvB, g[t], nrb, nta);
		for (int b = 0; b < NBLK; b++) {
			blo[b] = WAVE_H;
			bhi[b] = -1;
		}
		for (int x = 0; x < WAVE_W; x++) {
			int ro = rb[t][x], rn = nrb[x];
			uint16_t *po = acc + ro * WAVE_W + x, *pn = acc + rn * WAVE_W + x;
			for (int k = 0; k < 4; k++, po += WAVE_W, pn += WAVE_W) {
				*po -= ta[t][x][k];
				*pn += nta[x][k];
			}
			memcpy(ta[t][x], nta[x], 4);
			rb[t][x] = (int16_t)rn;
			int a = ro < rn ? ro : rn, b = (ro > rn ? ro : rn) + 3;
			for (int y = a; y <= b; y++) {
				int i = y * WAVE_W + x;
				dst[i] = wave_lighten(bg[i], acc[i]);
			}
			int bi = x / BLK;
			blo[bi] = a < blo[bi] ? a : blo[bi];
			bhi[bi] = b > bhi[bi] ? b : bhi[bi];
		}
		emit(dirty, WAVE_MAX_DIRTY / nu);
	}
}

const wave_t wave_silk = { "Шёлк", init, render };
