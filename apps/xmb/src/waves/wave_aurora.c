/* Северное сияние: мягкие «занавеси» — вертикальные лучи с яркой нижней кромкой и долгим затуханием вверх.
 * Колышутся медленно (несколько синусов по таблице); музыка делает лучи ярче и выше (спектр — по ширине, басы слева),
 * высокие слегка мерцают отдельными лучами. Столбцы обновляются группами по CW, каждая — раз в NPH кадров,
 * пиксели с прежней яркостью не трогаются, dirty — по границам реально изменившихся пикселей группы. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define CW 8                    /* ширина группы столбцов */
#define NCH (WAVE_W / CW)
#define NPH 3                   /* группа обновляется раз в NPH кадров */
#define PROF 64                 /* длина таблиц профиля */
#define AMAX 170                /* потолок осветления (из 256) */

static int16_t sin_tab[1024];   /* -256..256 */
static uint16_t prof_up[PROF], prof_dn[PROF];
static uint8_t hsh[WAVE_W];
static uint8_t alpha[WAVE_W * WAVE_H];
static int16_t col_top[WAVE_W], col_bot[WAVE_W];
static unsigned fno;
static int have;

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
}

static inline unsigned tph(uint64_t ms, unsigned w16)
{
	return (unsigned)((ms * w16) >> 10);
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	int full = f->full || !have;
	uint64_t ms = (uint64_t)(f->t * 1000.0);
	unsigned p1 = tph(ms, 3855), p2 = tph(ms, 5958), p3 = tph(ms, 2730), p4 = tph(ms, 9362);
	unsigned p5 = tph(ms, 12372), p6 = tph(ms, 196608), p7 = tph(ms, 4681), p8 = tph(ms, 1638);
	int spi[WAVE_BANDS + 1];
	for (int b = 0; b < WAVE_BANDS; b++) {
		int v = (int)(f->spec[b] * 256.0f);
		spi[b] = v < 0 ? 0 : v > 256 ? 256 : v;
	}
	spi[WAVE_BANDS] = spi[WAVE_BANDS - 1];
	int L = (int)(f->level * 256.0f);
	L = L < 0 ? 0 : L > 256 ? 256 : L;
	int bass = (spi[0] + spi[1] + spi[2] + spi[3]) >> 2;

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
		if (!full && ((c + fno) % NPH))
			continue;
		int x0 = c * CW;
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
			int gain = 150 + (L * 40 >> 8) + (sp * 100 >> 8) + (bass * 40 >> 8);
			int a = ((env * ray >> 8) * mult >> 8) * gain >> 8;
			a = a > AMAX ? AMAX : a;
			int up = 34 + (a >> 2) + (sp * 30 >> 8) + (L * 14 >> 8);
			up = up > 100 ? 100 : up;
			int dn = 10 + (a >> 4) + (sp * 8 >> 8);
			cy[i] = 90 - (L * 10 >> 8) + (SIN(x * 190 + p7) * 14 >> 8) + (e * 8 >> 8);
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
