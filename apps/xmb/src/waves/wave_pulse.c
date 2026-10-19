/* Волна «Пульс»: спектр → плавная светящаяся кривая через всю ширину (кубическая интерполяция между
 * полосами), мягкое свечение в несколько слоёв и бледное отражение вниз. Музыка (wave_music.c): форму в
 * основном дают полосы в своём размахе (громкий мастеринг не упирает кривую в потолок), кривая дышит на
 * сглаженном басу — растёт и светлеет; каждая бочка пускает по линии удар пульса — горб с крутым фронтом и
 * пологим хвостом бежит слева направо, где он прошёл, линия ярче; малый барабан — горб поменьше и уже,
 * бежит навстречу, справа налево; хэт — короткие искры на линии (только яркость). В тишине — медленное
 * «дыхание». Всё целочисленное, кривая — в четвертях пикселя.
 *
 * Каждый кадр: высота и яркость кривой по столбцам; столбец, где они не изменились, не трогаем. У
 * изменившегося перерисовываем только строки от старой до новой позиции (с запасом на свечение и отражение).
 * На крутых склонах столбец тянется вертикально до середины соседей, чтобы линия не рассыпалась.
 * Dirty — по группам столбцов; группы склеиваются, пока прямоугольников не станет <= WAVE_MAX_DIRTY. */
#include <math.h>
#include <string.h>

#include "wave.h"

#define BASE_Y 76                 /* строка основания: кривая растёт вверх, отражение уходит вниз */
#define BASE_Q (BASE_Y * 4)
#define BASE2_Q (2 * BASE_Q)
#define HMAX_Q 264                /* высота при spec = 1 (четверти пикселя) */
#define HCAP_Q 284
#define R 5                       /* опорная строка в таблице профиля */
#define NROW (2 * R + 2)          /* строк в таблице профиля */
#define NLVL 10                   /* 0 — тишина, 1 — музыка, 2..9 — ярче: бас, удары, искры */
#define CSH 2
#define NCH (WAVE_W >> CSH)       /* групп столбцов по 4 */
#define BAND_W (WAVE_W / WAVE_BANDS)
#define MASK 0x07E0F81Fu
#define KF 12                     /* горб бочки: фронт и хвост, px */
#define KB 32
#define KH_Q 120                  /* его высота при силе 1, четверти пикселя */
#define NSN 3                     /* горбов малого барабана */
#define SR 10                     /* их полуширина, px */
#define SH_Q 60
#define SV 420.0f                 /* их скорость, px/с */
#define NSP 6                     /* искр хэта */

typedef struct {
	int x1, x2, qmin, qmax;
} seg_t;

typedef struct {
	const uint8_t *ta, *tb;   /* профиль у верхнего и нижнего края участка */
	int ba, bb;               /* строка, которой соответствует индекс 0 */
	int in_lo, in_hi, core;   /* строки внутри участка — сплошная яркость */
} zone_t;

static uint8_t ctab[NLVL][4][NROW], rtab[NLVL][4][NROW];  /* альфа (0..32) по уровню, дробной части, строке */
static int up_c, dn_c, up_r, dn_r;                         /* реальный вылет свечения кривой/отражения */
static int16_t cr_w[WAVE_W][4];                            /* веса Catmull-Rom, Q10 */
static uint8_t jx[WAVE_W];
static uint16_t hlut[1024];
static int16_t sintab[1024];
static int tab_ready;

static uint8_t kprof[KB + KF + 1], sprof[2 * SR + 1];             /* профили горбов, 0..255 */
static int osa[WAVE_W], osb[WAVE_W], ny[WAVE_W + 2], breath[WAVE_W];
static uint8_t olv[WAVE_W];
static int have_prev, last_tq;
static wave_pulses_t pl;                /* импульсы бочки */
static int pls[WAVE_W];                 /* их свечение по столбцам, 0..256 */
static int kh[WAVE_W], lb[WAVE_W];      /* по столбцам: высота горбов (четверти пикселя) и прибавка яркости */
static float nb[WAVE_BANDS];            /* полосы разбора, сглаженные как спектр */
static float snx[NSN], sna[NSN], spx[NSP], spa[NSP];
static unsigned last_sn, last_hat;
static uint32_t rnd_s = 2463534242u;

static uint32_t rnd(void)
{
	rnd_s ^= rnd_s << 13;
	rnd_s ^= rnd_s >> 17;
	rnd_s ^= rnd_s << 5;
	return rnd_s;
}

static void init(void)
{
	if (!tab_ready) {
		static const float sc[NLVL] = { 0.55f, 0.64f, 0.74f, 0.84f, 0.94f, 1.04f, 1.15f, 1.26f, 1.38f, 1.5f };
		int cmin = NROW, cmax = -1, rmin = NROW, rmax = -1;
		for (int l = 0; l < NLVL; l++)
			for (int fr = 0; fr < 4; fr++)
				for (int i = 0; i < NROW; i++) {
					float d = i - R - fr / 4.0f;
					float p = 0.60f * expf(-d * d / 0.64f) + 0.36f * expf(-d * d / 3.2f) +
					          0.16f * expf(-d * d / 10.0f);
					int a = (int)(32 * sc[l] * p + 0.5f), b = (int)(32 * sc[l] * 0.32f * p + 0.5f);
					ctab[l][fr][i] = a > 32 ? 32 : a;
					rtab[l][fr][i] = b > 32 ? 32 : b;
					if (a) {
						cmin = i < cmin ? i : cmin;
						cmax = i > cmax ? i : cmax;
					}
					if (b) {
						rmin = i < rmin ? i : rmin;
						rmax = i > rmax ? i : rmax;
					}
				}
		up_c = R - cmin;
		dn_c = cmax - R;
		up_r = R - rmin;
		dn_r = rmax - R;
		for (int x = 0; x < WAVE_W; x++) {
			float t = ((x + BAND_W / 2) % BAND_W) / (float)BAND_W, t2 = t * t, t3 = t2 * t;
			float w[4] = { (-t3 + 2 * t2 - t) / 2, (3 * t3 - 5 * t2 + 2) / 2, (-3 * t3 + 4 * t2 + t) / 2,
			               (t3 - t2) / 2 };
			int sum = 0;
			for (int k = 0; k < 4; k++)
				sum += cr_w[x][k] = (int16_t)lroundf(w[k] * 1024);
			cr_w[x][1] += 1024 - sum;
			jx[x] = (uint8_t)((x + BAND_W / 2) / BAND_W);
		}
		for (int v = 0; v < 1024; v++)
			hlut[v] = (uint16_t)(HMAX_Q * powf(v / 1023.0f, 0.75f) + 0.5f);
		for (int i = 0; i < 1024; i++)
			sintab[i] = (int16_t)lroundf(256 * sinf(i * 6.2831853f / 1024));
		for (int d = -KB; d <= KF; d++) {
			float u = d > 0 ? d / (float)KF : -d / (float)KB;
			kprof[d + KB] = (uint8_t)lroundf(255 * expf(-2.5f * u * u));
		}
		for (int d = -SR; d <= SR; d++) {
			float u = d / (float)SR;
			sprof[d + SR] = (uint8_t)lroundf(255 * expf(-3 * u * u));
		}
		tab_ready = 1;
	}
	have_prev = 0;
	last_tq = -1;
	memset(&pl, 0, sizeof(pl));
	memset(nb, 0, sizeof(nb));
	memset(sna, 0, sizeof(sna));
	memset(spa, 0, sizeof(spa));
	last_sn = last_hat = 0;
}

/* удары по столбцам: kh — высота горбов, lb — прибавка яркости (0..256 и выше) */
static void bumps(const wave_music_t *m, float dt)
{
	wave_pulses_update(&pl, m, dt, pls);
	for (int x = 0; x < WAVE_W; x++) {
		kh[x] = 0;
		lb[x] = pls[x];
	}
	for (int i = 0; i < WAVE_NPULSE; i++) {     /* горб бочки — уже и острее свечения */
		if (pl.a[i] < 0.02f)
			continue;
		int a = (int)(pl.a[i] * KH_Q), hx = (int)pl.x[i];
		for (int x = hx - KB < 0 ? 0 : hx - KB; x < WAVE_W && x <= hx + KF; x++)
			kh[x] += (a * kprof[x - hx + KB]) >> 8;
	}
	if (m->snares != last_sn) {
		last_sn = m->snares;
		int k = 0;
		for (int i = 1; i < NSN; i++)
			if (sna[i] < sna[k])
				k = i;
		snx[k] = WAVE_W + SR;
		sna[k] = m->snare;
	}
	for (int i = 0; i < NSN; i++) {
		if (sna[i] < 0.02f)
			continue;
		snx[i] -= SV * dt;
		sna[i] *= expf(-dt / 0.6f);
		if (snx[i] < -SR) {
			sna[i] = 0;
			continue;
		}
		int h = (int)(sna[i] * SH_Q), l = (int)(sna[i] * 150), hx = (int)snx[i];
		for (int x = hx - SR < 0 ? 0 : hx - SR; x < WAVE_W && x <= hx + SR; x++) {
			kh[x] += (h * sprof[x - hx + SR]) >> 8;
			lb[x] += (l * sprof[x - hx + SR]) >> 8;
		}
	}
	if (m->hats != last_hat) {
		last_hat = m->hats;
		for (int n = 0; n < 2; n++) {
			int k = 0;
			for (int i = 1; i < NSP; i++)
				if (spa[i] < spa[k])
					k = i;
			spx[k] = 8 + (float)(rnd() % (WAVE_W - 16));
			spa[k] = m->hat;
		}
	}
	for (int i = 0; i < NSP; i++) {
		if (spa[i] < 0.05f)
			continue;
		spa[i] *= expf(-dt / 0.14f);
		int l = (int)(spa[i] * 220), hx = (int)spx[i];
		for (int d = -4; d <= 4; d++)
			lb[hx + d] += (l * (5 - (d < 0 ? -d : d))) / 5;
	}
}

/* осветлить RGB565 к белому на a/32 */
static inline uint16_t blend(uint16_t p, int a)
{
	uint32_t s = (p | ((uint32_t)p << 16)) & MASK;
	s = (s + (((MASK - s) * (uint32_t)a) >> 5)) & MASK;
	return (uint16_t)(s | (s >> 16));
}

/* участок [sa, sb] (четверти пикселя) → зона: профиль у краёв, сплошной центр */
static inline void zone_set(zone_t *z, const uint8_t (*tab)[NROW], int sa, int sb)
{
	z->ta = tab[sa & 3];
	z->tb = tab[sb & 3];
	z->ba = (sa >> 2) - R;
	z->bb = (sb >> 2) - R;
	z->in_lo = (sa + 3) >> 2;
	z->in_hi = sb >> 2;
	z->core = tab[0][R];
}

static inline int zone_a(const zone_t *z, int y)
{
	if (y >= z->in_lo && y <= z->in_hi)
		return z->core;
	unsigned i = (unsigned)(y - z->ba), j = (unsigned)(y - z->bb);
	int a = i < NROW ? z->ta[i] : 0, b = j < NROW ? z->tb[j] : 0;
	return a > b ? a : b;
}

static inline void paint(uint16_t *dst, const uint16_t *bg, int x, int lo, int hi, const zone_t *c, const zone_t *r)
{
	if (lo < 0)
		lo = 0;
	if (hi > WAVE_H - 1)
		hi = WAVE_H - 1;
	for (int y = lo; y <= hi; y++) {
		int a = zone_a(c, y) + zone_a(r, y);
		int k = y * WAVE_W + x;
		dst[k] = a ? blend(bg[k], a > 32 ? 32 : a) : bg[k];
	}
}

static inline int seg_h(const seg_t *s)
{
	return (s->qmax >> 2) - (s->qmin >> 2) + up_c + dn_c + 1;
}

/* строки прямоугольников группы: кривая [clo, chi], отражение [rlo, rhi] */
static inline void seg_rows(const seg_t *s, int *clo, int *chi, int *rlo, int *rhi)
{
	*clo = (s->qmin >> 2) - up_c;
	*chi = (s->qmax >> 2) + dn_c;
	*rlo = ((BASE2_Q - s->qmax) >> 2) - up_r;
	*rhi = ((BASE2_Q - s->qmin) >> 2) + dn_r;
}

static inline int seg_rects(const seg_t *s)
{
	int clo, chi, rlo, rhi;
	seg_rows(s, &clo, &chi, &rlo, &rhi);
	return rlo <= chi + 1 ? 1 : 2;
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	int full = f->full || !have_prev;
	static const wave_music_t calm;
	const wave_music_t *m = f->m ? f->m : &calm;
	float dt = f->dt > 0.1 ? 0.1f : (float)f->dt;
	float ka = dt * 14 > 1 ? 1 : dt * 14, kd = dt * 5 > 1 ? 1 : dt * 5;
	float nbs[WAVE_BANDS + 2];
	for (int b = 0; b < WAVE_BANDS; b++)
		nbs[b + 1] = nb[b] += (m->band[b] - nb[b]) * (m->band[b] > nb[b] ? ka : kd);
	nbs[0] = nbs[1];
	nbs[WAVE_BANDS + 1] = nbs[WAVE_BANDS];
	int a[WAVE_BANDS + 4];
	float total = 0;
	for (int b = 0; b < WAVE_BANDS; b++) {
		float s = f->spec[b];
		total += s;
		/* громкий мастеринг упирает спектр в потолок — форму даёт полоса в своём размахе (без зубцов между
		 * соседними полосами) */
		if (f->m)
			s = 0.4f * s + 0.6f * (nbs[b] + 2 * nbs[b + 1] + nbs[b + 2]) * 0.25f;
		int v = (int)(s * (1023.0f + b * 13.0f));   /* лёгкий подъём высоких */
		a[b + 2] = v > 1023 ? 1023 : v < 0 ? 0 : v;
	}
	a[0] = a[1] = a[2];
	a[WAVE_BANDS + 3] = a[WAVE_BANDS + 2] = a[WAVE_BANDS + 1];

	bumps(m, dt);
	int silent = f->level < 0.01f && total < 0.3f;
	/* с разбором музыки кривая ниже — запас под горбы; бас её раскрывает */
	int ba = (int)(m->bass_att * 256);
	int gain = f->m ? 160 + ((ba * 80) >> 8) : 256, lvb = (ba * 400) >> 8;

	/* дыхание — обновляем ~11 раз в секунду, чтобы в тишине кадры были пустыми */
	int tq = (int)(f->t * 11.25);
	if (full || tq != last_tq) {
		for (int x = 0; x < WAVE_W; x++) {
			int p1 = x * 307 + tq * 641, p2 = x * 614 - tq * 408;
			breath[x] = 8 + ((7 * sintab[(p1 >> 6) & 1023]) >> 8) + ((4 * sintab[(p2 >> 6) & 1023]) >> 8);
		}
		last_tq = tq;
	}

	for (int x = 0; x < WAVE_W; x++) {
		const int16_t *w = cr_w[x];
		const int *p = a + jx[x];
		int v = (w[0] * p[0] + w[1] * p[1] + w[2] * p[2] + w[3] * p[3]) >> 10;
		v = v < 0 ? 0 : v > 1023 ? 1023 : v;
		int h = ((hlut[v] * gain) >> 8) + kh[x] + breath[x];
		ny[x + 1] = BASE_Q - (h < 0 ? 0 : h > HCAP_Q ? HCAP_Q : h);
	}
	ny[0] = ny[1];
	ny[WAVE_W + 1] = ny[WAVE_W];

	if (full)
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
	int cmin[NCH], cmax[NCH];
	for (int c = 0; c < NCH; c++) {
		cmin[c] = 1 << 20;
		cmax[c] = -1;
	}
	for (int x = 0; x < WAVE_W; x++) {
		int y0 = ny[x + 1], l = (y0 + ny[x]) >> 1, r = (y0 + ny[x + 2]) >> 1;
		int sa = y0 < l ? (y0 < r ? y0 : r) : (l < r ? l : r);
		int sb = y0 > l ? (y0 > r ? y0 : r) : (l > r ? l : r);
		int lv = 0;
		if (!silent) {
			lv = 1 + ((lvb + lb[x] * 8) >> 8);
			lv = lv > NLVL - 1 ? NLVL - 1 : lv;
		}
		if (!full && lv == olv[x] && sa == osa[x] && sb == osb[x])
			continue;
		olv[x] = (uint8_t)lv;
		int qmin = full ? sa : (sa < osa[x] ? sa : osa[x]), qmax = full ? sb : (sb > osb[x] ? sb : osb[x]);
		osa[x] = sa;
		osb[x] = sb;
		zone_t zc, zr;
		zone_set(&zc, ctab[lv], sa, sb);
		zone_set(&zr, rtab[lv], BASE2_Q - sb, BASE2_Q - sa);
		int clo = (qmin >> 2) - up_c, chi = (qmax >> 2) + dn_c;
		int rlo = ((BASE2_Q - qmax) >> 2) - up_r, rhi = ((BASE2_Q - qmin) >> 2) + dn_r;
		if (rlo <= chi + 1) {
			paint(dst, bg, x, clo, rhi, &zc, &zr);
		} else {
			paint(dst, bg, x, clo, chi, &zc, &zr);
			paint(dst, bg, x, rlo, rhi, &zc, &zr);
		}
		int c = x >> CSH;
		cmin[c] = qmin < cmin[c] ? qmin : cmin[c];
		cmax[c] = qmax > cmax[c] ? qmax : cmax[c];
	}
	have_prev = 1;

	if (full) {
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		return;
	}
	seg_t sg[NCH];
	int n = 0, rects = 0;
	for (int c = 0; c < NCH; c++)
		if (cmax[c] >= 0) {
			sg[n] = (seg_t){ c << CSH, (c << CSH) + (1 << CSH) - 1, cmin[c], cmax[c] };
			rects += seg_rects(&sg[n]);
			n++;
		}
	while (rects > WAVE_MAX_DIRTY && n > 1) {   /* склеить соседей с наименьшим приростом площади */
		int best = 0, bc = 1 << 30;
		for (int i = 0; i + 1 < n; i++) {
			seg_t m = { sg[i].x1, sg[i + 1].x2, sg[i].qmin < sg[i + 1].qmin ? sg[i].qmin : sg[i + 1].qmin,
				    sg[i].qmax > sg[i + 1].qmax ? sg[i].qmax : sg[i + 1].qmax };
			int cost = (m.x2 - m.x1 + 1) * seg_h(&m) - (sg[i].x2 - sg[i].x1 + 1) * seg_h(&sg[i]) -
			           (sg[i + 1].x2 - sg[i + 1].x1 + 1) * seg_h(&sg[i + 1]);
			if (cost < bc) {
				bc = cost;
				best = i;
			}
		}
		rects -= seg_rects(&sg[best]) + seg_rects(&sg[best + 1]);
		sg[best].x2 = sg[best + 1].x2;
		sg[best].qmin = sg[best].qmin < sg[best + 1].qmin ? sg[best].qmin : sg[best + 1].qmin;
		sg[best].qmax = sg[best].qmax > sg[best + 1].qmax ? sg[best].qmax : sg[best + 1].qmax;
		rects += seg_rects(&sg[best]);
		memmove(&sg[best + 1], &sg[best + 2], sizeof(seg_t) * (n - best - 2));
		n--;
	}
	for (int i = 0; i < n; i++) {
		int clo, chi, rlo, rhi;
		seg_rows(&sg[i], &clo, &chi, &rlo, &rhi);
		if (rlo <= chi + 1) {
			wave_dirty_add(dirty, sg[i].x1, clo, sg[i].x2, rhi);
		} else {
			wave_dirty_add(dirty, sg[i].x1, clo, sg[i].x2, chi);
			wave_dirty_add(dirty, sg[i].x1, rlo, sg[i].x2, rhi);
		}
	}
}

const wave_t wave_pulse = { "Пульс", init, render };
