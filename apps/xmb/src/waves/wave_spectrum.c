/* Волна «Спектр»: 32 столбика (басы слева) от центральной линии вверх, под ней — бледное зеркальное
 * отражение. Высота в 1/16 пикселя: верх столбика сглажен по покрытию строки и скруглён по углам, поэтому
 * движется без «ступенек». Яркость растёт вверх и привязана к строке (не к столбцу), так что при смене
 * высоты перекрашиваются только строки между старым и новым верхом. Пик — тонкая светлая черта над
 * столбиком: держится ~0.25 с, дальше опускается с ускорением. В тишине столбики садятся в короткую
 * едва заметную линию, пики гаснут по пути вниз.
 * Музыка (wave_music.c): высота — полоса в своём недавнем размахе (полный ход на тихой записи и на громком
 * мастеринге) с долей сглаженного спектра для формы; вверх — сразу, вниз — плавно. Каждая бочка пускает по
 * столбикам импульс слева направо: где он проходит, столбики подпрыгивают и пики вспыхивают; хэт зажигает пики
 * верхов.
 *
 * Каждый кадр: новая высота и пик по столбцам; столбик, где они не изменились, не трогаем. У изменившегося
 * перерисовываем только затронутые строки (верх/пик и отражение), прямоугольники склеиваем по наименьшему
 * приросту площади, пока их не станет <= WAVE_MAX_DIRTY. Целочисленно, без выделения памяти. */
#include <math.h>
#include <string.h>

#include "wave.h"

#define PITCH 10                  /* шаг столбиков; столбик 8 px, зазор по 1 px с боков */
#define BAR_W 8
#define BAR_X0 1
#define BASE 102                  /* строка центральной линии: столбик над ней, отражение под ней */
#define HMIN 16                   /* высота в тишине, 1/16 px */
#define HMAX 960                  /* 60 px */
#define KROWS 96
#define MROWS 48
#define REFL_N 7                  /* высота отражения = h * 7 / 16 */
#define REFL_D 44                 /* строк, за которые отражение гаснет полностью */
#define A_BOT 22                  /* яркость столбика (0..256) у основания и у верха: под ним текст меню */
#define A_TOP 72
#define A_REFL 22                 /* яркость отражения у основания */
#define PK_GAP 32                 /* пик над столбиком, 1/16 px */
#define PK_T 24
#define PK_A 95
#define HOLD_US 250000
#define VEL0 40960                /* пиксели в Q12: 10 px/с, ускорение 200 px/с² */
#define ACC 819200
#define FALL 0.12f                /* спад высоты, с */
#define DEAD 3                    /* мёртвая зона высоты, 1/16 px */
#define IT_MAX (WAVE_BANDS * 4)

typedef struct {
	int h, pk;                /* показанные высота и пик, 1/16 px */
	int ppos, vel, hold;      /* физика пика: положение Q12 px, скорость, остаток удержания, мкс */
	int lv;                   /* сглаженная цель высоты, 1/256 от 1/16 px */
	int fl;                   /* вспышка пика, 0..256 */
} bar_t;

static bar_t bars[WAVE_BANDS];
static uint16_t hlut[1024];
static uint16_t gain[WAVE_BANDS];
static uint8_t ga[KROWS], gr[MROWS];
static const uint8_t cr[BAR_W] = { 21, 6, 1, 0, 0, 1, 6, 21 };   /* скругление верха, 1/16 px */
static int tab_ready, have_prev;
static wave_pulses_t pl;          /* импульсы бочки по столбикам */
static int pls[WAVE_W];           /* их сумма по столбцам, 0..256 */

static void init(void)
{
	if (!tab_ready) {
		for (int v = 0; v < 1024; v++)
			hlut[v] = (uint16_t)(HMIN + (HMAX - HMIN) * powf(v / 1023.0f, 0.8f) + 0.5f);
		for (int b = 0; b < WAVE_BANDS; b++)
			gain[b] = (uint16_t)(1023.0f * (1.0f + 0.012f * b) + 0.5f);
		for (int k = 0; k < KROWS; k++) {
			float t = k * 16.0f / HMAX;
			t = t > 1 ? 1 : t;
			ga[k] = (uint8_t)(A_BOT + (A_TOP - A_BOT) * t + 0.5f);
		}
		for (int m = 0; m < MROWS; m++) {
			float t = 1.0f - (float)m / REFL_D;
			gr[m] = (uint8_t)(t > 0 ? A_REFL * t + 0.5f : 0);
		}
		tab_ready = 1;
	}
	memset(bars, 0, sizeof(bars));
	for (int i = 0; i < WAVE_BANDS; i++) {
		bars[i].h = bars[i].pk = HMIN;
		bars[i].ppos = HMIN << 8;
		bars[i].lv = HMIN << 8;
	}
	memset(&pl, 0, sizeof(pl));
	have_prev = 0;
}

static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

/* строки столбика i выше центральной линии: k от klo до khi (0 — нижняя), тело и пик */
static void paint_up(uint16_t *dst, const uint16_t *bg, int i, int klo, int khi)
{
	const bar_t *b = &bars[i];
	int hc[BAR_W], ps = b->pk + PK_GAP, pe = ps + PK_T;
	for (int c = 0; c < BAR_W; c++)
		hc[c] = b->h - imin(cr[c], b->h >> 1);
	int pa = b->pk > HMIN ? imin(256, ((b->pk - HMIN) * 21) >> 2) : 0;
	pa = (pa * (PK_A + b->fl * (256 - PK_A) / 256)) >> 8;
	int pe_a = (pa * 154) >> 8;                       /* края черты слабее */
	khi = imin(khi, KROWS - 1);
	for (int k = imax(klo, 0); k <= khi; k++) {
		int y = BASE - 1 - k, kk = k << 4;
		if (y < 0)
			break;
		int ap = 0, ae = 0;
		if (pa) {
			int ov = imin(pe, kk + 16) - imax(ps, kk);
			if (ov > 0) {
				ap = (pa * ov) >> 4;
				ae = (pe_a * ov) >> 4;
			}
		}
		uint16_t *d = dst + y * WAVE_W + i * PITCH + BAR_X0;
		const uint16_t *s = bg + y * WAVE_W + i * PITCH + BAR_X0;
		for (int c = 0; c < BAR_W; c++) {
			int v = hc[c] - kk, a = v >= 16 ? ga[k] : v > 0 ? (ga[k] * v) >> 4 : 0;
			int p = (c == 0 || c == BAR_W - 1) ? ae : ap;
			a = p > a ? p : a;
			d[c] = a ? wave_lighten(s[c], a) : s[c];
		}
	}
}

/* отражение столбика i: строки m от mlo до mhi (0 — верхняя, у центральной линии) */
static void paint_refl(uint16_t *dst, const uint16_t *bg, int i, int mlo, int mhi)
{
	int rh = (bars[i].h * REFL_N) >> 4, hc[BAR_W];
	for (int c = 0; c < BAR_W; c++)
		hc[c] = rh - imin(cr[c], rh >> 1);
	mhi = imin(mhi, MROWS - 1);
	for (int m = imax(mlo, 0); m <= mhi; m++) {
		int y = BASE + 1 + m, mm = m << 4;
		if (y >= WAVE_H)
			break;
		uint16_t *d = dst + y * WAVE_W + i * PITCH + BAR_X0;
		const uint16_t *s = bg + y * WAVE_W + i * PITCH + BAR_X0;
		for (int c = 0; c < BAR_W; c++) {
			int v = hc[c] - mm, a = v >= 16 ? gr[m] : v > 0 ? (gr[m] * v) >> 4 : 0;
			d[c] = a ? wave_lighten(s[c], a) : s[c];
		}
	}
}

typedef struct {
	int x1, y1, x2, y2;
} item_t;

static inline int item_area(const item_t *a)
{
	return (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1);
}

/* прирост площади при склейке a и b */
static inline int merge_cost(const item_t *a, const item_t *b)
{
	item_t u = { imin(a->x1, b->x1), imin(a->y1, b->y1), imax(a->x2, b->x2), imax(a->y2, b->y2) };
	return item_area(&u) - item_area(a) - item_area(b);
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	int full = f->full || !have_prev;
	int dt_us = (int)(f->dt * 1e6f), dtq = (int)(f->dt * 65536.0f);
	dt_us = imin(imax(dt_us, 0), 100000);
	dtq = imin(imax(dtq, 0), 6553);
	int oh[WAVE_BANDS], op[WAVE_BANDS], of[WAVE_BANDS];
	static const wave_music_t calm;
	const wave_music_t *m = f->m ? f->m : &calm;
	wave_pulses_update(&pl, m, f->dt > 0.1 ? 0.1f : (float)f->dt, pls);
	int fq = (int)(f->dt * (65536.0f / FALL));
	fq = imin(imax(fq, 0), 65536);

	for (int i = 0; i < WAVE_BANDS; i++) {
		bar_t *b = &bars[i];
		oh[i] = b->h;
		op[i] = b->pk;
		of[i] = b->fl;
		/* цель: полоса в своём размахе + немного спектра для формы; импульс бочки подбрасывает — пропорционально,
		 * чтобы форма спектра сохранялась */
		int pv = pls[i * PITCH + BAR_X0 + BAR_W / 2];
		float x = f->spec[i] * gain[i] * (1.0f / 1023);
		if (f->m) {
			/* обычный уровень полосы — около середины размаха: он внизу, над ним — ход до верха */
			x = (0.85f * m->band[i] + 0.15f * x - 0.3f) * 1.43f;
			x = x < 0 ? 0 : x * (0.5f + 0.42f * x);
		}
		int v = ((int)(x * 1023) * (256 + (pv >> 1))) >> 8;
		int nh = hlut[v < 0 ? 0 : v > 1023 ? 1023 : v] << 8;
		/* вверх — сразу, вниз — плавно */
		b->lv = nh >= b->lv ? nh : b->lv - (int)(((int64_t)(b->lv - nh) * fq) >> 16);
		nh = b->lv >> 8;
		/* вспышка пика: где идёт импульс, и на хэте у верхов */
		int fl = ((pv * 200) >> 8) + (int)(m->hat * (i > 16 ? (i - 16) * 12 : 0));
		b->fl = imin(fl, 256) & ~15;
		int d = nh - b->h;
		if (d >= DEAD || d <= -DEAD || (d && (nh == HMIN || nh == HMAX)))
			b->h = nh;
		/* пик: прилипает к столбику, держится, затем падает с ускорением */
		if (b->h << 8 >= b->ppos) {
			b->ppos = b->h << 8;
			b->hold = HOLD_US;
			b->vel = VEL0;
		} else if (b->hold > 0) {
			b->hold -= dt_us;
		} else {
			b->vel += (int)(((int64_t)ACC * dtq) >> 16);
			b->ppos -= (int)(((int64_t)b->vel * dtq) >> 16);
			if (b->ppos < b->h << 8)
				b->ppos = b->h << 8;
		}
		b->pk = b->ppos >> 8;
	}
	have_prev = 1;

	if (full) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		for (int i = 0; i < WAVE_BANDS; i++) {
			paint_up(dst, bg, i, 0, (bars[i].pk + PK_GAP + PK_T - 1) >> 4);
			paint_refl(dst, bg, i, 0, (((bars[i].h * REFL_N) >> 4) - 1) >> 4);
		}
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		return;
	}

	item_t it[IT_MAX], rt[WAVE_BANDS];
	int nu = 0, nr = 0;
	for (int i = 0; i < WAVE_BANDS; i++) {
		int h = bars[i].h, pk = bars[i].pk, x1 = i * PITCH + BAR_X0, x2 = x1 + BAR_W - 1;
		int dh = h != oh[i], dp = pk != op[i] || bars[i].fl != of[i];
		if (!dh && !dp)
			continue;
		int lo[3], hi[3], n = 0;               /* строки выше центра (k), перекрашиваемые у этого столбика */
		if (dh) {
			lo[n] = imax((imin(h, oh[i]) - 21) >> 4, 0);
			hi[n++] = (imax(h, oh[i]) - 1) >> 4;
		}
		if (dp) {
			lo[n] = (op[i] + PK_GAP) >> 4;
			hi[n++] = (op[i] + PK_GAP + PK_T - 1) >> 4;
			lo[n] = (pk + PK_GAP) >> 4;
			hi[n++] = (pk + PK_GAP + PK_T - 1) >> 4;
		}
		for (int a = 1; a < n; a++)            /* по возрастанию lo */
			for (int c = a; c > 0 && lo[c] < lo[c - 1]; c--) {
				int t = lo[c]; lo[c] = lo[c - 1]; lo[c - 1] = t;
				t = hi[c]; hi[c] = hi[c - 1]; hi[c - 1] = t;
			}
		for (int a = 0; a < n;) {              /* близкие диапазоны — одним куском */
			int l = lo[a], r = hi[a];
			for (a++; a < n && lo[a] <= r + 4; a++)
				r = imax(r, hi[a]);
			paint_up(dst, bg, i, l, r);
			it[nu++] = (item_t){ x1, BASE - 1 - r, x2, BASE - 1 - l };
		}
		if (dh) {
			int rh0 = (h * REFL_N) >> 4, rh1 = (oh[i] * REFL_N) >> 4;
			int l = imax((imin(rh0, rh1) - 21) >> 4, 0), r = (imax(rh0, rh1) - 1) >> 4;
			paint_refl(dst, bg, i, l, r);
			rt[nr++] = (item_t){ x1, BASE + 1 + l, x2, BASE + 1 + r };
		}
	}
	memcpy(it + nu, rt, sizeof(item_t) * nr);
	int n = nu + nr;
	if (!n)
		return;

	/* список в порядке добавления: склеиваем соседей с наименьшим приростом площади */
	int nx[IT_MAX], pv[IT_MAX], cst[IT_MAX], cnt = n;
	for (int i = 0; i < n; i++) {
		nx[i] = i + 1 < n ? i + 1 : -1;
		pv[i] = i - 1;
		cst[i] = nx[i] >= 0 ? merge_cost(&it[i], &it[nx[i]]) : 0x7fffffff;
	}
	while (cnt > WAVE_MAX_DIRTY) {
		int best = -1, bc = 0x7fffffff;
		for (int i = 0; i >= 0; i = nx[i])
			if (cst[i] < bc) {
				bc = cst[i];
				best = i;
			}
		if (best < 0)
			break;
		int j = nx[best];
		it[best] = (item_t){ imin(it[best].x1, it[j].x1), imin(it[best].y1, it[j].y1),
		                     imax(it[best].x2, it[j].x2), imax(it[best].y2, it[j].y2) };
		nx[best] = nx[j];
		if (nx[j] >= 0)
			pv[nx[j]] = best;
		cst[best] = nx[best] >= 0 ? merge_cost(&it[best], &it[nx[best]]) : 0x7fffffff;
		if (pv[best] >= 0)
			cst[pv[best]] = merge_cost(&it[pv[best]], &it[best]);
		cnt--;
	}
	for (int i = 0; i >= 0; i = nx[i])
		wave_dirty_add(dirty, it[i].x1, it[i].y1, it[i].x2, it[i].y2);
}

const wave_t wave_spectrum = { "Спектр", init, render };
