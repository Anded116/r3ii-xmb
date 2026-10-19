/* Северное сияние: мягкие «занавеси» — вертикальные лучи с яркой нижней кромкой и долгим затуханием вверх.
 * Колышутся медленно (несколько синусов по таблице). Музыка (wave_music.c): занавесь дышит на сглаженном басу —
 * лучи выше и ярче, кромка чуть поднимается; каждая бочка пускает вдоль дуги всплеск — где он проходит, лучи
 * вытягиваются вверх и светлеют, и это место бежит слева направо, как волна яркости по настоящему сиянию;
 * складки и лучи плывут быстрее, когда играет плотно, и с рывком на бочке; середина углубляет складки, на хэтах
 * вспыхивают и гаснут отдельные лучи; спектр по ширине (басы слева) — местная яркость и мерцание лучей.
 * В тишине — исходное спокойное колыхание.
 *
 * Столбцы обновляются группами по CW, каждая — раз в NPH кадров; группа, куда только что набежал фронт всплеска,
 * — в этот же кадр. Пиксели с прежней яркостью не трогаются, dirty — по границам реально изменившихся пикселей
 * группы. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define CW 8                    /* ширина группы столбцов */
#define NCH (WAVE_W / CW)
#define NPH 3                   /* группа обновляется раз в NPH кадров */
#define PROF 64                 /* длина таблиц профиля */
#define AMAX 170                /* потолок осветления (из 256) */
#define KEYD 64                 /* рост всплеска в группе, после которого она рисуется вне очереди, 0..256 */

static int16_t sin_tab[1024];   /* -256..256 */
static uint16_t prof_up[PROF], prof_dn[PROF];
static uint8_t hsh[WAVE_W];
static uint8_t alpha[WAVE_W * WAVE_H];
static int16_t col_top[WAVE_W], col_bot[WAVE_W];
static unsigned fno;
static int have;
static double ft;               /* время течения: идёт быстрее, когда играет плотно */
static wave_pulses_t pl;        /* всплески бочки вдоль дуги */
static int pls[WAVE_W];         /* их сумма по столбцам, 0..256 */
static int bs[WAVE_BANDS];      /* полосы спектра, сглаженные, 0..256 */
static uint8_t fla[WAVE_W];     /* вспыхнувшие на хэте лучи, 0..255 */
static int gkey[NCH];           /* всплеск в группе при её прошлой отрисовке */
static unsigned last_hat, rnd;

#define SIN(p) ((int)sin_tab[((unsigned)(p) >> 6) & 1023])

static void init(void)
{
	unsigned s = 12345;
	for (int i = 0; i < 1024; i++)
		sin_tab[i] = (int16_t)lround(256 * sin(2 * M_PI * i / 1024));
	for (int i = 0; i < PROF; i++) {
		double u = (double)i / (PROF - 1), w = 0.5 * (1 + cos(M_PI * u));
		prof_up[i] = (uint16_t)lround(256 * pow(w, 1.1));
		prof_dn[i] = (uint16_t)lround(256 * pow(w, 2.2));
	}
	for (int i = 0; i < WAVE_W; i++) {
		s = s * 1103515245u + 12345u;
		hsh[i] = (uint8_t)(s >> 16);
	}
	have = 0;
	fno = 0;
	ft = 0;
	memset(&pl, 0, sizeof(pl));
	memset(bs, 0, sizeof(bs));
	memset(fla, 0, sizeof(fla));
	rnd = 777;
}

static inline unsigned tph(uint64_t ms, unsigned w16)
{
	return (unsigned)((ms * w16) >> 10);
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	static const wave_music_t calm;
	const wave_music_t *m = f->m ? f->m : &calm;
	int full = f->full || !have;
	float dt = f->dt > 0.1 ? 0.1f : (float)f->dt;
	ft += dt * (1 + 1.3f * m->energy + 2.4f * m->kick);
	uint64_t ms = (uint64_t)(ft * 1000.0), mt = (uint64_t)(f->t * 1000.0);
	unsigned p1 = tph(ms, 3855), p2 = tph(ms, 5958), p3 = tph(ms, 2730), p4 = tph(ms, 9362);
	unsigned p5 = tph(ms, 12372), p6 = tph(mt, 196608), p7 = tph(ms, 4681), p8 = tph(ms, 1638);
	wave_pulses_update(&pl, m, dt, pls);
	int spi[WAVE_BANDS + 1];
	for (int b = 0; b < WAVE_BANDS; b++) {
		int v = (int)(m->band[b] * 256.0f);
		v = v < 0 ? 0 : v > 256 ? 256 : v;
		bs[b] += v > bs[b] ? (v - bs[b]) >> 1 : (v - bs[b]) >> 3;
		spi[b] = bs[b];
	}
	spi[WAVE_BANDS] = spi[WAVE_BANDS - 1];
	int B = (int)(m->bass_att * 256), M = (int)(m->mid_att * 256), E = (int)(m->energy * 256);
	for (int x = 0; x < WAVE_W; x++)
		fla[x] -= (fla[x] >> 2) + (fla[x] > 0);
	if (m->hats != last_hat) {
		last_hat = m->hats;
		int v = 130 + (int)(m->hat * 125);
		for (int n = 2 + (int)(m->hat * 3); n > 0; n--) {
			rnd = rnd * 1103515245u + 12345u;
			int x = (int)((rnd >> 16) % WAVE_W), w = 1 + ((rnd >> 12) & 1);
			for (int i = -w; i <= w; i++) {
				int xi = x + i, u = i ? v * 2 / 3 : v;
				if (xi >= 0 && xi < WAVE_W && u > fla[xi])
					fla[xi] = (uint8_t)u;
			}
		}
	}

	if (full) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		memset(alpha, 0, sizeof(alpha));
		for (int x = 0; x < WAVE_W; x++) {
			col_top[x] = WAVE_H;
			col_bot[x] = -1;
		}
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
	}

	for (int c = 0; c < NCH; c++) {
		int x0 = c * CW, key = (pls[x0] + pls[x0 + CW - 1]) >> 1;
		if (!full && ((c + fno) % NPH) && key - gkey[c] < KEYD)
			continue;
		gkey[c] = key;
		int cy[CW], A[CW], ihu[CW], ihd[CW], ctr[CW], ctb[CW];
		int top[CW], bot[CW];
		int lo = WAVE_H, hi = -1;
		for (int i = 0; i < CW; i++) {
			int x = x0 + i;
			/* огибающая складок и тонкая структура лучей */
			int e = (SIN(x * 266 + p1) * 5 + SIN(x * 553 - p2) * 3 + SIN(x * 143 + p3) * 2) / 10;
			int env = (e + 256) >> 1;
			env = env * env >> 8;
			env = 36 + (env * 220 >> 8);
			int r = (SIN(x * 7282 + p4) + SIN(x * 12337 - p5)) >> 1;
			int ray = 96 + ((r + 256) * 5 >> 4);
			/* спектр по ширине, с интерполяцией между полосами */
			int pos = (x * 205 >> 3) - 128;
			if (pos < 0)
				pos = 0;
			int b0 = pos >> 8, fr = pos & 255;
			int sp = (spi[b0] * (256 - fr) + spi[b0 + 1] * fr) >> 8;
			int xs = x * 205 >> 8;
			int shim = sp * xs >> 8;
			int mult = 256 + (SIN(hsh[x] * 256 + p6) * shim >> 9);
			int pk = pls[x] * (512 - pls[x]) >> 8, fl = fla[x];
			/* всплеск бочки и вспыхнувший луч светлеют сильнее спокойного потолка, но ненадолго */
			int gain = 150 + (E * 15 >> 8) + (B * 25 >> 8) + (sp * 35 >> 8) + (pk * 190 >> 8) + (fl * 170 >> 8);
			int a = ((env * ray >> 8) * mult >> 8) * gain >> 8;
			int amax = AMAX + ((pk + fl) * 60 >> 8);
			a = a > amax ? amax : a;
			int up = 34 + (a >> 2) + (sp * 10 >> 8) + (B * 12 >> 8) + (pk * 40 >> 8) + (fl * 26 >> 8);
			up = up > 108 ? 108 : up;
			int dn = 10 + (a >> 4) + (sp * 6 >> 8) + (pk * 6 >> 8);
			cy[i] = 90 - (B * 8 >> 8) - (pk * 7 >> 8) + (SIN(x * 190 + p7) * 14 >> 8) + (e * (8 + (M * 10 >> 8)) >> 8);
			A[i] = a & ~3;
			ihu[i] = 256 * (PROF - 1) / up;
			ihd[i] = 256 * (PROF - 1) / dn;
			int h = SIN(x * 330 + p8);
			ctr[i] = 190 + (h * 30 >> 8);
			ctb[i] = 225 - (h * 30 >> 8);
			if (A[i]) {
				top[i] = cy[i] - up < 0 ? 0 : cy[i] - up;
				bot[i] = cy[i] + dn > WAVE_H - 1 ? WAVE_H - 1 : cy[i] + dn;
			} else {
				top[i] = WAVE_H;
				bot[i] = -1;
			}
			int t = top[i] < col_top[x] ? top[i] : col_top[x];
			int bt = bot[i] > col_bot[x] ? bot[i] : col_bot[x];
			lo = t < lo ? t : lo;
			hi = bt > hi ? bt : hi;
			col_top[x] = (int16_t)top[i];
			col_bot[x] = (int16_t)bot[i];
		}
		int mnx = WAVE_W, mxx = -1, mny = WAVE_H, mxy = -1;
		for (int y = lo; y <= hi; y++) {
			uint16_t *dp = dst + y * WAVE_W + x0;
			const uint16_t *bp = bg + y * WAVE_W + x0;
			uint8_t *ap = alpha + y * WAVE_W + x0;
			for (int i = 0; i < CW; i++) {
				int a = 0, d = cy[i] - y, idx;
				if (A[i]) {
					if (d >= 0) {
						idx = (d * ihu[i]) >> 8;
						if (idx < PROF)
							a = A[i] * prof_up[idx] >> 8;
					} else {
						idx = (-d * ihd[i]) >> 8;
						if (idx < PROF)
							a = A[i] * prof_dn[idx] >> 8;
					}
					a &= ~3;
				}
				if (a == ap[i])
					continue;
				ap[i] = (uint8_t)a;
				uint16_t p = bp[i];
				if (a) {
					int pr = p >> 11, pg = (p >> 5) & 63, pb = p & 31;
					pr += ((31 - pr) * (a * ctr[i] >> 8)) >> 8;
					pg += ((63 - pg) * a) >> 8;
					pb += ((31 - pb) * (a * ctb[i] >> 8)) >> 8;
					p = (uint16_t)((pr << 11) | (pg << 5) | pb);
				}
				dp[i] = p;
				if (x0 + i < mnx) mnx = x0 + i;
				if (x0 + i > mxx) mxx = x0 + i;
				if (y < mny) mny = y;
				if (y > mxy) mxy = y;
			}
		}
		if (!full && mxx >= 0)
			wave_dirty_add(dirty, mnx, mny, mxx, mxy);
	}
	fno++;
	have = 1;
}

const wave_t wave_aurora = { "Сияние", init, render };
