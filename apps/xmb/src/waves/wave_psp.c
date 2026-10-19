/* Волна PSP: три мягких полупрозрачных светлых листа один за другим, как в XMB у PSP. У каждого листа —
 * широкая плавная кромка (две синусоиды по таблице, медленный дрейф, фазы слегка плывут), сверху по кромке —
 * тонкая светлая линия с бликом там, где лист «смотрит» на свет, под кромкой — короткий отсвет и заливка.
 * Заливка зависит только от строки (затухает книзу до нуля у нижнего края полосы), поэтому при сдвиге кромки
 * меняется лишь узкая зона вокруг неё: профиль листа от расстояния до кромки ненулевой в пределах -UP..+DN px,
 * глубже — постоянная заливка строки. Музыка (wave_music.c): листы дышат на сглаженном басу — размах больше,
 * сами чуть поднимаются; каждая бочка пускает по ленте импульс — передний лист в этом месте вспухает вверх,
 * кромки всех листов светлеют, и этот вал бежит слева направо; течение быстрее, когда играет плотно, и с рывком
 * на бочке; на хэтах по кромкам вспыхивают и гаснут блики.
 * В тишине — исходное спокойное течение.
 *
 * Кадр: у каждого листа и столбца — строки вокруг старой и новой кромки, если она сдвинулась хотя бы на HYS/16 px
 * или заметно сменилась яркость кромки;
 * пиксель пересчитывается по всем листам, пишется, только если изменился. dirty — по блокам столбцов от первой
 * до последней изменившейся строки; соседние блоки жадно склеиваются по наименьшему приросту площади, пока
 * прямоугольников больше WAVE_MAX_DIRTY. Всё целочисленное:
 * кромка в 1/16 px, синус и профиль — таблицы; дизеринг 4×4 убирает полосы на плавной заливке. */
#include <math.h>
#include <string.h>

#include "wave.h"

#define NL 3                    /* листов */
#define SUB 16                  /* долей пикселя в координате кромки */
#define UP 3                    /* профиль: строк над кромкой */
#define DN 5                    /* и под ней */
#define PN ((UP + DN) * SUB)
#define BLK 2                   /* гранулярность прямоугольников, столбцов */
#define MERGE 8                 /* склеивать и при меньшем числе прямоугольников, если прирост площади не больше */
#define NBLK (WAVE_W / BLK)
#define HYS 8                   /* столбец не трогаем, пока кромка сдвинулась меньше, 1/16 px */
#define RHYS 16                 /* и пока яркость кромки сменилась меньше, из 256 */
#define SNB 12                  /* таблица синуса: 4096 значений */
#define NGL 3                   /* бликов на хэт */
#define GLR 12                  /* полуширина блика, столбцов */
#define SWR 12                  /* полуширина сглаживания вала, столбцов */

typedef struct {
	double ct;                  /* средняя высота кромки, px от верха полосы */
	double a1, k1, v1;          /* основная синусоида: размах px, волн на ширину, оборотов фазы в секунду */
	double a2, k2, v2;          /* вторая, мельче */
	double ph, wob;             /* начальная фаза (обороты) и частота её покачивания (Гц) */
	int fill, rim, sheen;       /* заливка, кромка, отсвет под кромкой (0..256) */
	int swell;                  /* вал от бочки: подъём кромки при полном импульсе, 1/16 px */
} sheet_t;

/* сзади вперёд: дальний лист бледнее и шире, передний — ярче кромкой */
static const sheet_t S[NL] = {
	{ 78, 14, 0.55, -0.030, 4.0, 1.30,  0.045, 0.10, 0.031, 30,  90, 20, 0 },
	{ 90, 16, 0.75,  0.042, 4.5, 1.70, -0.050, 0.45, 0.023, 34, 130, 26, 0 },
	{ 102, 12, 0.95, -0.055, 3.5, 2.20,  0.065, 0.80, 0.037, 40, 170, 32, 14 * SUB },
};

static int16_t sn[1 << SNB];            /* sin, ±16384 */
static uint8_t cov[PN];                 /* покрытие пикселя листом по расстоянию до кромки, 0..255 */
static uint8_t rimt[PN];                /* светлая линия кромки */
static uint8_t sht[PN];                 /* отсвет под кромкой */
static uint8_t g[WAVE_H];               /* затухание заливки книзу, 0..255 */
static int fl[NL][WAVE_H];              /* заливка листа по строке */
static int ed[NL][WAVE_W];              /* нарисованная кромка, 1/16 px */
static int rs[NL][WAVE_W];              /* нарисованная яркость кромки (с бликом), 0..256 */
static int ne[WAVE_W + 2], nr[WAVE_W];
static int blo[NBLK], bhi[NBLK];
static int ready;
static double ft;                       /* время течения: идёт быстрее, когда играет плотно */
static wave_pulses_t pl;                /* импульсы бочки по ленте */
static int pls[WAVE_W];                 /* их сумма по столбцам, 0..256 */
static uint8_t gl[NL][WAVE_W];          /* блики хэтов на кромках, 0..255 */
static uint8_t glp[2 * GLR + 1];         /* профиль блика */
static int mb, me;                      /* бас (сглаженный) и плотность, 0..256 */
static unsigned last_hat, rnd;

static const uint8_t bay[4][4] = {
	{ 8, 136, 40, 168 }, { 200, 72, 232, 104 }, { 56, 184, 24, 152 }, { 248, 120, 216, 88 },
};

static inline double sstep(double x)
{
	x = x < 0 ? 0 : x > 1 ? 1 : x;
	return x * x * (3 - 2 * x);
}

static void init(void)
{
	for (int i = 0; i < (1 << SNB); i++)
		sn[i] = (int16_t)lround(16384 * sin(2 * M_PI * (i + 0.5) / (1 << SNB)));
	for (int i = 0; i < PN; i++) {
		double d = (i + 0.5) / SUB - UP;        /* центр пикселя относительно кромки, px (вниз +) */
		double c = 0, r = 0, s = 0;
		for (int j = 0; j < 8; j++) {           /* усреднение по высоте пикселя */
			double u = d - 0.5 + (j + 0.5) / 8;
			c += u > 0 ? 1 : 0;
			r += exp(-(u - 0.2) * (u - 0.2) / (2 * 0.55 * 0.55));
			s += u > 0 ? exp(-u / 1.3) : 0;
		}
		c /= 8, r /= 8, s /= 8;
		double tail = 1 - sstep((d - (DN - 2.5)) / 2.0);        /* к краю профиля — ровно в ноль */
		cov[i] = (uint8_t)lround(255 * c);
		rimt[i] = (uint8_t)lround(255 * r * (d < 0 ? 1 - sstep((-d - (UP - 1.2)) / 1.0) : tail));
		sht[i] = (uint8_t)lround(255 * s * tail);
	}
	for (int y = 0; y < WAVE_H; y++)
		g[y] = (uint8_t)lround(255 * pow(1 - sstep((y - 70.0) / (WAVE_H - 1 - 70.0)), 0.7));
	for (int k = 0; k < NL; k++)
		for (int y = 0; y < WAVE_H; y++)
			fl[k][y] = (S[k].fill * g[y] + 127) / 255;
	for (int i = 0; i <= 2 * GLR; i++)
		glp[i] = (uint8_t)lround(255 * exp(-(i - GLR) * (i - GLR) / (2 * 4.0 * 4.0)));
	ready = 0;
	ft = 0;
	memset(&pl, 0, sizeof(pl));
	memset(gl, 0, sizeof(gl));
	rnd = 4242;
}

static inline uint32_t turns32(double x)
{
	x -= floor(x);
	return (uint32_t)(uint64_t)(x * 4294967296.0);
}

/* новая кромка листа k (ne, 1/16 px) и яркость кромки по столбцам (nr) */
static void sheet_calc(int k, double t)
{
	const sheet_t *h = &S[k];
	double pw = 0.06 * sin(2 * M_PI * h->wob * t);
	uint32_t p1 = turns32(h->ph + h->v1 * ft + pw), p2 = turns32(-1.3 * h->ph + h->v2 * ft - 1.6 * pw);
	uint32_t d1 = (uint32_t)(h->k1 * 4294967296.0 / WAVE_W), d2 = (uint32_t)(h->k2 * 4294967296.0 / WAVE_W);
	double br = 1 + 0.10 * sin(2 * M_PI * (0.017 * t + 0.3 * k));    /* медленное «дыхание» размаха */
	int am = 256 + ((mb * 80 + me * 40) >> 8);                         /* дыхание: до +45% размаха */
	int A1 = (int)(h->a1 * SUB * br), A2 = (int)(h->a2 * SUB * br);
	A1 = (A1 * am) >> 8;
	A2 = (A2 * am) >> 8;
	int c = (int)(h->ct * SUB) - ((mb * 4 * SUB) >> 8);
	for (int x = 0; x < WAVE_W; x++) {
		ne[x + 1] = c + ((A1 * sn[p1 >> (32 - SNB)]) >> 14) + ((A2 * sn[p2 >> (32 - SNB)]) >> 14)
			- ((pls[x] * h->swell) >> 8);
		p1 += d1;
		p2 += d2;
	}
	ne[0] = 2 * ne[1] - ne[2];
	ne[WAVE_W + 1] = 2 * ne[WAVE_W] - ne[WAVE_W - 1];
	int rim = h->rim + ((me * 30) >> 8);
	for (int x = 0; x < WAVE_W; x++) {
		/* наклон в 1/16 px на 2 столбца; блик — где кромка слегка поднимается вправо (свет сверху-слева) */
		int s = ne[x + 2] - ne[x] + 4;
		s = s < 0 ? -s : s;
		int sp = 256 - s * 26;
		sp = sp < 0 ? 0 : sp;
		sp = (sp * sp) >> 8;
		int v = ((rim * (150 + ((sp * 106) >> 8))) >> 8) + ((pls[x] * 110) >> 8) + gl[k][x];
		nr[x] = v > 256 ? 256 : v;
	}
}

static inline uint16_t lit(uint16_t p, int a, int dt)
{
	if (a <= 0)
		return p;
	if (a > 256)
		a = 256;
	int r = p >> 11, gg = (p >> 5) & 63, b = p & 31;
	r += ((31 - r) * a + dt) >> 8;
	gg += ((63 - gg) * a + dt) >> 8;
	b += ((31 - b) * a + dt) >> 8;
	return (uint16_t)((r << 11) | (gg << 5) | b);
}

/* пиксель по нарисованному состоянию всех листов */
static inline uint16_t pix(const uint16_t *bg, int x, int y)
{
	int a = 0, yc = y * SUB + SUB / 2;
	for (int k = 0; k < NL; k++) {
		int i = yc - ed[k][x] + UP * SUB;
		if (i < 0)
			continue;
		if (i >= PN)
			a += fl[k][y];
		else
			a += ((cov[i] * fl[k][y]) >> 8) + ((rimt[i] * rs[k][x]) >> 8) + ((sht[i] * S[k].sheen) >> 8);
	}
	return lit(bg[y * WAVE_W + x], a, bay[y & 3][x & 3]);
}

/* прямоугольники: по одному на непустой блок, затем жадно склеиваются соседние с наименьшим приростом
 * площади — пока их больше WAVE_MAX_DIRTY или прирост совсем мал */
static inline int mcost(const wave_rect_t *a, const wave_rect_t *b)
{
	int y1 = a->y1 < b->y1 ? a->y1 : b->y1, y2 = a->y2 > b->y2 ? a->y2 : b->y2;
	return (b->x2 - a->x1 + 1) * (y2 - y1 + 1) - (a->x2 - a->x1 + 1) * (a->y2 - a->y1 + 1)
		- (b->x2 - b->x1 + 1) * (b->y2 - b->y1 + 1);
}

static void emit(wave_dirty_t *d)
{
	wave_rect_t r[NBLK];
	int c[NBLK], n = 0;
	for (int b = 0; b < NBLK; b++)
		if (blo[b] <= bhi[b])
			r[n++] = (wave_rect_t){ b * BLK, blo[b], b * BLK + BLK - 1, bhi[b] };
	for (int i = 0; i + 1 < n; i++)
		c[i] = mcost(&r[i], &r[i + 1]);
	while (n > 1) {
		int best = 0;
		for (int i = 1; i + 1 < n; i++)
			if (c[i] < c[best])
				best = i;
		if (n <= WAVE_MAX_DIRTY && c[best] > MERGE)
			break;
		wave_rect_t *a = &r[best], *b = &r[best + 1];
		a->x2 = b->x2;
		a->y1 = b->y1 < a->y1 ? b->y1 : a->y1;
		a->y2 = b->y2 > a->y2 ? b->y2 : a->y2;
		memmove(b, b + 1, sizeof(*b) * (n - best - 2));
		memmove(&c[best], &c[best + 1], sizeof(int) * (n - best - 2));
		n--;
		if (best > 0)
			c[best - 1] = mcost(&r[best - 1], &r[best]);
		if (best + 1 < n)
			c[best] = mcost(&r[best], &r[best + 1]);
	}
	for (int i = 0; i < n; i++)
		wave_dirty_add(d, r[i].x1, r[i].y1, r[i].x2, r[i].y2);
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	static const wave_music_t calm;
	const wave_music_t *m = f->m ? f->m : &calm;
	float dt = f->dt > 0.1 ? 0.1f : (float)f->dt;
	ft += dt * (1 + 0.9f * m->energy + 1.6f * m->kick);
	mb = (int)(m->bass_att * 256);
	me = (int)(m->energy * 256);
	wave_pulses_update(&pl, m, dt, pls);
	for (int x = 0; x < WAVE_W; x++)        /* импульс поднимает сразу и заметно, хвост — мягко */
		pls[x] = (pls[x] * (512 - pls[x])) >> 8;
	for (int pass = 0; pass < 2; pass++) {  /* вал — пологий холм, а не складка: дважды скользящее среднее */
		int ps[WAVE_W + 1];
		ps[0] = 0;
		for (int x = 0; x < WAVE_W; x++)
			ps[x + 1] = ps[x] + pls[x];
		for (int x = 0; x < WAVE_W; x++) {
			int a = x - SWR < 0 ? 0 : x - SWR, b = x + SWR + 1 > WAVE_W ? WAVE_W : x + SWR + 1;
			pls[x] = (ps[b] - ps[a]) / (2 * SWR + 1);
		}
	}
	for (int k = 0; k < NL; k++)
		for (int x = 0; x < WAVE_W; x++)
			gl[k][x] -= (gl[k][x] >> 3) + (gl[k][x] > 0);
	if (m->hats != last_hat) {
		last_hat = m->hats;
		int a = 110 + (int)(m->hat * 145);
		for (int n = 0; n < NGL; n++) {
			rnd = rnd * 1103515245u + 12345u;
			int k = (int)((rnd >> 8) % NL), x0 = (int)((rnd >> 16) % WAVE_W) - GLR;
			for (int i = 0; i <= 2 * GLR; i++) {
				int x = x0 + i, v = (a * glp[i]) >> 8;
				if (x >= 0 && x < WAVE_W && v > gl[k][x])
					gl[k][x] = (uint8_t)v;
			}
		}
	}

	if (f->full || !ready) {
		for (int k = 0; k < NL; k++) {
			sheet_calc(k, f->t);
			memcpy(ed[k], ne + 1, sizeof(ed[k]));
			memcpy(rs[k], nr, sizeof(rs[k]));
		}
		for (int x = 0; x < WAVE_W; x++) {
			int top = ed[0][x];             /* выше всех листов — чистый фон */
			for (int k = 1; k < NL; k++)
				top = ed[k][x] < top ? ed[k][x] : top;
			top = (top - UP * SUB) >> 4;
			top = top < 0 ? 0 : top > WAVE_H ? WAVE_H : top;
			for (int y = 0; y < top; y++)
				dst[y * WAVE_W + x] = bg[y * WAVE_W + x];
			for (int y = top; y < WAVE_H; y++)
				dst[y * WAVE_W + x] = pix(bg, x, y);
		}
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
		ready = 1;
		return;
	}

	for (int b = 0; b < NBLK; b++) {
		blo[b] = WAVE_H;
		bhi[b] = -1;
	}
	for (int k = 0; k < NL; k++) {
		sheet_calc(k, f->t);
		for (int x = 0; x < WAVE_W; x++) {
			int eo = ed[k][x], en = ne[x + 1];
			int de = en - eo, dr = nr[x] - rs[k][x];
			if (de < HYS && de > -HYS && dr < RHYS && dr > -RHYS)
				continue;
			ed[k][x] = en;
			rs[k][x] = nr[x];
			int a = (eo < en ? eo : en) - UP * SUB, z = (eo > en ? eo : en) + DN * SUB;
			int y0 = a >> 4, y1 = (z >> 4) + 1;
			y0 = y0 < 0 ? 0 : y0;
			y1 = y1 > WAVE_H - 1 ? WAVE_H - 1 : y1;
			int lo = WAVE_H, hi = -1;
			for (int y = y0; y <= y1; y++) {
				uint16_t p = pix(bg, x, y);
				uint16_t *q = &dst[y * WAVE_W + x];
				if (*q != p) {
					*q = p;
					lo = y < lo ? y : lo;
					hi = y;
				}
			}
			int bi = x / BLK;
			blo[bi] = lo < blo[bi] ? lo : blo[bi];
			bhi[bi] = hi > bhi[bi] ? hi : bhi[bi];
		}
	}
	emit(dirty);
}

const wave_t wave_psp = { "Волна PSP", init, render };
