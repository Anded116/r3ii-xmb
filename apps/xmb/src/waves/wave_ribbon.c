/* Лента: две полупрозрачные ленты (заливка между верхней и нижней кромкой) с яркой сглаженной верхней
 * кромкой; вторая бледнее и плывёт в другую сторону. Кромки — суммы синусоид по таблице (целые 8.8 px),
 * спектр локально поднимает верх, утолщает ленту и подсвечивает кромку; амплитуды ограничены, лента всегда
 * целиком в полосе. Заливка плоская, поэтому при сдвиге формы меняются только края: у каждого столбца храним
 * положение кромок и перерисовываем зону вокруг них, лишь когда сдвиг больше порога. Ленты обновляются по
 * очереди (раз в 2 кадра). al — яркость каждой ленты в пикселе, пиксель = фон, осветлённый на сумму.
 * dirty — прямоугольники по блокам столбцов отдельно для верхней и нижней кромки (по 12 штук). */
#include <math.h>
#include <string.h>

#include "wave.h"

#define NR 2                    /* лент */
#define BLK 4                   /* гранулярность границ прямоугольников, столбцов */
#define NBLK (WAVE_W / BLK)
#define GAP 2                   /* пустых блоков подряд, которые прямоугольник ещё перекрывает */
#define THR 20                  /* порог сдвига кромки для перерисовки, 8.8 px */
#define ETHR 6                  /* то же для яркости кромки */
#define TOPN 192                /* профиль верха: от -3 до +9 px от кромки, шаг 1/16 */
#define BOTN 80                 /* профиль низа: ±2.5 px, шаг 1/16 */
#define MINTH (16 * 256)        /* минимальная толщина ленты */
#define YMIN (14 * 256)         /* границы, внутри которых кромки лежат всегда */
#define YMAX ((WAVE_H - 14) * 256)

typedef struct {
	double ct, a1, k1, w1, p1, a2, k2, w2, p2;      /* верх: центр, две синусоиды (px, циклов на ширину, об/с, фаза) */
	double th, ta, k3, w3, p3;                      /* толщина: средняя, размах и её синусоида */
	double lt, lb;                                  /* подъём верха и опускание низа от спектра, px */
	int fill, e0, ek, el, sheen;                    /* заливка, кромка: база, от спектра, от уровня, блик (0..255) */
} rib_t;

static const rib_t P[NR] = {
	{ 52, 9, 0.9, 0.050, 0.10, 4.5, 2.1, -0.070, 0.40,   32, 7, 1.4,  0.040, 0.30,  15, 10,  38, 95, 95, 30, 26 },
	{ 80, 8, 1.2, -0.035, 0.65, 4.0, 2.6,  0.060, 0.15,   30, 6, 1.7, -0.045, 0.70,  12,  9,  24, 52, 62, 18, 18 },
};

typedef struct {
	int ct, a1, a2, th, ta, lt, lb;
	uint32_t dk1, dk2, dk3;
	double p1, w1, p2, w2, p3, w3;
} rt_t;

static rt_t rt[NR];
static int16_t sn[1025];                /* sin, ±16384 */
static struct { uint8_t c, g; } tl[TOPN];       /* верх: покрытие заливкой и блик кромки */
static uint8_t bl[BOTN];                /* низ: покрытие заливкой */
static int cb[WAVE_W], cw[WAVE_W];      /* столбец -> полоса спектра и сглаженная дробь до следующей */
static int cs[WAVE_W];                  /* спектр по столбцам, 0..256 (на кадр) */
static int sb[WAVE_BANDS];              /* спектр, сглаженный по времени, 0..256 */
static int lvs;                         /* уровень, сглаженный, 0..256 */
static int tt[NR][WAVE_W], bb[NR][WAVE_W], ee[NR][WAVE_W];     /* нарисованные верх, низ (8.8 px), яркость кромки */
static uint8_t al[NR][WAVE_W * WAVE_H];
static int lo[2][NBLK], hi[2][NBLK];
static int ready, fc;

static inline double sstep(double x)
{
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x * x * (3 - 2 * x);
}

static void init(void)
{
	for (int i = 0; i <= 1024; i++)
		sn[i] = (int16_t)lround(16384 * sin(2 * M_PI * i / 1024));
	for (int i = 0; i < TOPN; i++) {
		double d = (i + 0.5) / 16 - 3, u = d - 0.4, g;
		if (u < 0)
			g = 1.45 * exp(-u * u / (2 * 0.6 * 0.6));
		else
			g = exp(-u * u / (2 * 0.7 * 0.7)) + 0.45 * exp(-u / 1.8);
		g *= 1 - sstep((d - 5) / 3);
		tl[i].c = (uint8_t)lround(255 * sstep((d + 1) / 2));
		tl[i].g = (uint8_t)lround(255 * g / 1.45);
	}
	for (int i = 0; i < BOTN; i++)
		bl[i] = (uint8_t)lround(255 * sstep(((i + 0.5 - 40) / 16 + 2) / 4));
	for (int x = 0; x < WAVE_W; x++) {
		double p = (x - 4.5) / 10;      /* центры полос — в серединах десяток столбцов */
		int b = (int)floor(p);
		cb[x] = b;
		cw[x] = (int)lround(256 * sstep(p - b));
	}
	for (int r = 0; r < NR; r++) {
		const rib_t *p = &P[r];
		rt[r] = (rt_t){ (int)(p->ct * 256), (int)(p->a1 * 256), (int)(p->a2 * 256), (int)(p->th * 256),
				(int)(p->ta * 256), (int)(p->lt * 256), (int)(p->lb * 256),
				(uint32_t)(p->k1 * 4294967296.0 / WAVE_W), (uint32_t)(p->k2 * 4294967296.0 / WAVE_W),
				(uint32_t)(p->k3 * 4294967296.0 / WAVE_W), p->p1, p->w1, p->p2, p->w2, p->p3, p->w3 };
	}
	memset(sb, 0, sizeof(sb));
	lvs = 0;
	ready = 0;
	fc = 0;
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

/* кромки и яркость ленты r по столбцам */
static void calc(int r, double t, int *T, int *B, int *E)
{
	const rt_t *h = &rt[r];
	const rib_t *p = &P[r];
	uint32_t p1 = turns32(h->p1 + h->w1 * t), p2 = turns32(h->p2 + h->w2 * t), p3 = turns32(h->p3 + h->w3 * t);
	for (int x = 0; x < WAVE_W; x++) {
		int s1 = ssin(p1), s2 = ssin(p2), s3 = ssin(p3), c = cs[x];
		p1 += h->dk1;
		p2 += h->dk2;
		p3 += h->dk3;
		int w = h->ct + ((h->a1 * s1) >> 14) + ((h->a2 * s2) >> 14);
		int top = w - ((c * h->lt) >> 8);
		int bot = w + h->th + ((h->ta * s3) >> 14) + ((c * h->lb) >> 8);
		top = top < YMIN ? YMIN : top;
		bot = bot < top + MINTH ? top + MINTH : bot;
		if (bot > YMAX) {
			bot = YMAX;
			top = top > bot - MINTH ? bot - MINTH : top;
		}
		int e = p->e0 + ((c * p->ek) >> 8) + ((lvs * p->el) >> 8) + ((s2 * p->sheen) >> 14);
		T[x] = top;
		B[x] = bot;
		E[x] = e < 0 ? 0 : e > 255 ? 255 : e;
	}
}

static inline int clampi(int v, int a, int b)
{
	return v < a ? a : v > b ? b : v;
}

/* яркость ленты r в строках ya..yb столбца x у верхней кромки T; dst != NULL — сразу и пиксели */
static void zone_top(int r, int x, int ya, int yb, int T, int E, uint16_t *dst, const uint16_t *bg)
{
	int fill = P[r].fill, idx = ((ya << 8) + 896 - T) >> 4;
	uint8_t *a = al[r] + ya * WAVE_W + x, *o = al[r ^ 1] + ya * WAVE_W + x;
	for (int y = ya; y <= yb; y++, idx += 16, a += WAVE_W, o += WAVE_W) {
		int k = clampi(idx, 0, TOPN - 1);
		int v = ((tl[k].c * fill) >> 8) + ((tl[k].g * E) >> 8);
		*a = (uint8_t)(v > 255 ? 255 : v);
		if (dst) {
			int i = y * WAVE_W + x, s = *a + *o;
			dst[i] = s ? wave_lighten(bg[i], s) : bg[i];
		}
	}
}

/* то же у нижней кромки B */
static void zone_bot(int r, int x, int ya, int yb, int B, uint16_t *dst, const uint16_t *bg)
{
	int fill = P[r].fill, idx = (B + 512 - (ya << 8)) >> 4;
	uint8_t *a = al[r] + ya * WAVE_W + x, *o = al[r ^ 1] + ya * WAVE_W + x;
	for (int y = ya; y <= yb; y++, idx -= 16, a += WAVE_W, o += WAVE_W) {
		*a = (uint8_t)((bl[clampi(idx, 0, BOTN - 1)] * fill) >> 8);
		if (dst) {
			int i = y * WAVE_W + x, s = *a + *o;
			dst[i] = s ? wave_lighten(bg[i], s) : bg[i];
		}
	}
}

/* прямоугольники по блокам столбцов: число штук при высоте не больше hl */
static int segs(const int *l, const int *h, int hl, int gap, int m, wave_dirty_t *d)
{
	int n = 0, open = 0, s = 0, last = 0, mn = 0, mx = 0;
	for (int b = 0; b < NBLK; b++) {
		if (h[b] < 0)
			continue;
		if (open) {
			int nmn = l[b] < mn ? l[b] : mn, nmx = h[b] > mx ? h[b] : mx;
			if (b - last - 1 <= gap && nmx - nmn + 1 <= hl) {
				mn = nmn;
				mx = nmx;
				last = b;
				continue;
			}
			n++;
			if (d)
				wave_dirty_add(d, s * BLK, mn, (last + 1) * BLK - 1, mx);
			else if (n > m)
				return n;
		}
		open = 1;
		s = last = b;
		mn = l[b];
		mx = h[b];
	}
	if (open) {
		n++;
		if (d)
			wave_dirty_add(d, s * BLK, mn, (last + 1) * BLK - 1, mx);
	}
	return n;
}

/* минимальная высота, при которой укладываемся в m прямоугольников */
static void emit(wave_dirty_t *d, const int *l, const int *h, int m)
{
	int gap = GAP, a = 1, b = WAVE_H;
	if (segs(l, h, WAVE_H, gap, m, NULL) > m)
		gap = NBLK;
	while (a < b) {
		int mid = (a + b) >> 1;
		if (segs(l, h, mid, gap, m, NULL) <= m)
			b = mid;
		else
			a = mid + 1;
	}
	segs(l, h, a, gap, m, d);
}

static void full(uint16_t *dst, const uint16_t *bg, double t)
{
	memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
	memset(al, 0, sizeof(al));
	for (int r = 0; r < NR; r++) {
		calc(r, t, tt[r], bb[r], ee[r]);
		int fv = (255 * P[r].fill) >> 8;
		for (int x = 0; x < WAVE_W; x++) {
			int ta = clampi((tt[r][x] >> 8) - 3, 0, WAVE_H - 1), tb = clampi((tt[r][x] >> 8) + 8, 0, WAVE_H - 1);
			int ba = clampi((bb[r][x] >> 8) - 2, 0, WAVE_H - 1), bz = clampi((bb[r][x] >> 8) + 2, 0, WAVE_H - 1);
			zone_top(r, x, ta, tb, tt[r][x], ee[r][x], NULL, NULL);
			for (int y = tb + 1; y < ba; y++)
				al[r][y * WAVE_W + x] = (uint8_t)fv;
			zone_bot(r, x, ba, bz, bb[r][x], NULL, NULL);
		}
	}
	for (int x = 0; x < WAVE_W; x++) {
		int ya = clampi((tt[0][x] >> 8) - 3, 0, WAVE_H - 1), yb = clampi((bb[0][x] >> 8) + 2, 0, WAVE_H - 1);
		int ya1 = clampi((tt[1][x] >> 8) - 3, 0, WAVE_H - 1), yb1 = clampi((bb[1][x] >> 8) + 2, 0, WAVE_H - 1);
		ya = ya1 < ya ? ya1 : ya;
		yb = yb1 > yb ? yb1 : yb;
		for (int y = ya; y <= yb; y++) {
			int i = y * WAVE_W + x, s = al[0][i] + al[1][i];
			if (s)
				dst[i] = wave_lighten(bg[i], s);
		}
	}
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	/* спектр: по времени, по полосам и плавно между полосами -> столбцы */
	int s[WAVE_BANDS + 2], sm[WAVE_BANDS + 2], s2[WAVE_BANDS + 2];
	for (int b = 0; b < WAVE_BANDS; b++) {
		int v = f->spec ? (int)(f->spec[b] * 256) : 0;
		v = v < 0 ? 0 : v > 256 ? 256 : v;
		sb[b] += ((v - sb[b]) * (v > sb[b] ? 154 : 51)) >> 8;
		s[b + 1] = sb[b];
	}
	s[0] = s[1];
	s[WAVE_BANDS + 1] = s[WAVE_BANDS];
	for (int b = 0; b < WAVE_BANDS + 2; b++)
		s2[b] = b == 0 || b == WAVE_BANDS + 1 ? s[b] : (s[b - 1] + 2 * s[b] + s[b + 1]) >> 2;
	for (int b = 0; b < WAVE_BANDS + 2; b++)
		sm[b] = b == 0 || b == WAVE_BANDS + 1 ? s2[b] : (s2[b - 1] + 2 * s2[b] + s2[b + 1]) >> 2;
	for (int x = 0; x < WAVE_W; x++) {      /* cb может быть -1: это s[0] */
		int b = cb[x] + 1;
		cs[x] = sm[b] + (((sm[b + 1] - sm[b]) * cw[x]) >> 8);
	}
	int lv = (int)(f->level * 256);
	lv = lv < 0 ? 0 : lv > 256 ? 256 : lv;
	lvs += ((lv - lvs) * (lv > lvs ? 100 : 40)) >> 8;

	if (f->full || !ready) {
		full(dst, bg, f->t);
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		ready = 1;
		fc = 0;
		return;
	}

	int r = ++fc & 1;
	int T[WAVE_W], B[WAVE_W], E[WAVE_W];
	calc(r, f->t, T, B, E);
	for (int z = 0; z < 2; z++)
		for (int b = 0; b < NBLK; b++) {
			lo[z][b] = WAVE_H;
			hi[z][b] = -1;
		}
	for (int x = 0; x < WAVE_W; x++) {
		int bi = x / BLK;
		int dt = T[x] - tt[r][x], de = E[x] - ee[r][x], db = B[x] - bb[r][x];
		if (dt >= THR || dt <= -THR || de >= ETHR || de <= -ETHR) {
			int a = tt[r][x] < T[x] ? tt[r][x] : T[x], b = tt[r][x] > T[x] ? tt[r][x] : T[x];
			int ya = clampi((a >> 8) - 3, 0, WAVE_H - 1), yb = clampi((b >> 8) + 8, 0, WAVE_H - 1);
			tt[r][x] = T[x];
			ee[r][x] = E[x];
			zone_top(r, x, ya, yb, T[x], E[x], dst, bg);
			lo[0][bi] = ya < lo[0][bi] ? ya : lo[0][bi];
			hi[0][bi] = yb > hi[0][bi] ? yb : hi[0][bi];
		}
		if (db >= THR || db <= -THR) {
			int a = bb[r][x] < B[x] ? bb[r][x] : B[x], b = bb[r][x] > B[x] ? bb[r][x] : B[x];
			int ya = clampi((a >> 8) - 2, 0, WAVE_H - 1), yb = clampi((b >> 8) + 2, 0, WAVE_H - 1);
			bb[r][x] = B[x];
			zone_bot(r, x, ya, yb, B[x], dst, bg);
			lo[1][bi] = ya < lo[1][bi] ? ya : lo[1][bi];
			hi[1][bi] = yb > hi[1][bi] ? yb : hi[1][bi];
		}
	}
	emit(dirty, lo[0], hi[0], WAVE_MAX_DIRTY / 2);
	emit(dirty, lo[1], hi[1], WAVE_MAX_DIRTY / 2);
}

const wave_t wave_ribbon = { "Лента", init, render };
