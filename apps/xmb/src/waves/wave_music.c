/* Разбор музыки для фонов — как у визуализаторов вроде MilkDrop: не абсолютные уровни, а относительно недавнего
 * прошлого. Полоса меряется в своём размахе (среднее за ~3 с — ~0,35, недавний пик — 1), поэтому тихая запись,
 * громкий мастеринг и спад спектра к верхам двигают картинку одинаково. Удары — по спектральному потоку: сумме
 * приростов полос группы (в дБ) к большему из двух прошлых кадров, выше плавающего порога (среднее потока + два
 * его разброса); после удара группа глуха на время рефрактерности. Всё на кадр интерфейса (45 Гц), 32 полосы —
 * доли миллисекунды. */
#include <math.h>
#include <string.h>

#include "wave.h"

#define GATE 0.1f       /* ниже −54 дБ — тишина, не нормировать шум */

typedef struct {
	int b0, b1;         /* полосы группы, включительно */
	float floor_;       /* минимальный поток для удара (дБ/60 на полосу) */
	float refr;         /* рефрактерность, с */
	float tau;          /* спад импульса, с */
} grp_t;

/* бочка 40–120 Гц, малый 180 Гц – 3 кГц (тело и шум), хэт 4,5–16 кГц */
static const grp_t G[3] = { { 0, 5, 0.035f, 0.12f, 0.20f }, { 8, 22, 0.025f, 0.10f, 0.15f }, { 24, 31, 0.025f, 0.07f, 0.09f } };

static struct {
	int ready;
	float avg[WAVE_BANDS], pk[WAVE_BANDS], p1[WAVE_BANDS], p2[WAVE_BANDS];
	float fm[3], fd[3], since[3];
} s;

static float ema(float v, float x, float dt, float tau)
{
	return v + (x - v) * (dt >= tau ? 1 : dt / tau);
}

void wave_music_update(wave_music_t *m, const float *raw, float dt)
{
	if (dt <= 0)
		return;
	if (dt > 0.1f)
		dt = 0.1f;
	if (!s.ready) {
		for (int b = 0; b < WAVE_BANDS; b++) {
			s.avg[b] = s.pk[b] = s.p1[b] = s.p2[b] = raw[b];
		}
		s.ready = 1;
	}
	int loud = 0;
	for (int b = 0; b < WAVE_BANDS; b++)
		loud += raw[b] > GATE;

	/* полосы: размах — от чуть ниже среднего до недавнего пика (пик спадает к среднему за ~2 с) */
	for (int b = 0; b < WAVE_BANDS; b++) {
		float v = raw[b];
		if (!loud) {
			m->band[b] = ema(m->band[b], 0, dt, 0.15f);
			continue;
		}
		s.avg[b] = ema(s.avg[b], v, dt, 3.0f);
		s.pk[b] = v > s.pk[b] ? v : ema(s.pk[b], s.avg[b], dt, 2.0f);
		float lo = s.avg[b] - 0.05f, hi = s.pk[b] > s.avg[b] + 0.1f ? s.pk[b] : s.avg[b] + 0.1f;
		float n = v < GATE ? 0 : (v - lo) / (hi - lo);
		m->band[b] = n < 0 ? 0 : n > 1 ? 1 : n;
	}

	/* группы */
	float g[3] = { 0 };
	static const int gb[4] = { 0, 7, 21, WAVE_BANDS };
	for (int k = 0; k < 3; k++) {
		for (int b = gb[k]; b < gb[k + 1]; b++)
			g[k] += m->band[b];
		g[k] /= gb[k + 1] - gb[k];
	}
	m->bass = g[0];
	m->mid = g[1];
	m->treb = g[2];
	m->bass_att = ema(m->bass_att, g[0], dt, g[0] > m->bass_att ? 0.25f : 0.6f);
	m->mid_att = ema(m->mid_att, g[1], dt, g[1] > m->mid_att ? 0.3f : 0.7f);
	m->treb_att = ema(m->treb_att, g[2], dt, g[2] > m->treb_att ? 0.25f : 0.6f);
	m->energy = (m->bass_att + m->mid_att + m->treb_att) / 3;

	/* удары */
	float *pulse[3] = { &m->kick, &m->snare, &m->hat };
	unsigned *cnt[3] = { &m->kicks, &m->snares, &m->hats };
	for (int k = 0; k < 3; k++) {
		const grp_t *gr = &G[k];
		float f = 0;
		for (int b = gr->b0; b <= gr->b1; b++) {
			float ref = s.p1[b] > s.p2[b] ? s.p1[b] : s.p2[b];
			if (raw[b] > ref && raw[b] > GATE)
				f += raw[b] - ref;
		}
		f /= gr->b1 - gr->b0 + 1;
		float thr = s.fm[k] + 2 * s.fd[k] + gr->floor_;
		s.since[k] += dt;
		*pulse[k] *= expf(-dt / gr->tau);
		if (loud && f > thr && s.since[k] >= gr->refr) {
			float a = 0.4f + 0.6f * (f - thr) / (thr + 0.02f);
			a = a > 1 ? 1 : a;
			if (a > *pulse[k])
				*pulse[k] = a;
			(*cnt[k])++;
			s.since[k] = 0;
		}
		/* порог плывёт за потоком; удары в него входят урезанными — серия не задирает его */
		float fc = f > thr ? thr : f;
		s.fm[k] = ema(s.fm[k], fc, dt, 1.0f);
		s.fd[k] = ema(s.fd[k], fabsf(fc - s.fm[k]), dt, 1.0f);
	}
	memcpy(s.p2, s.p1, sizeof(s.p1));
	memcpy(s.p1, raw, sizeof(s.p1));
}

#define PV 600.0f       /* скорость импульса, px/с */
#define BACK 150        /* длина хвоста, px */
#define FRONT 40

void wave_pulses_update(wave_pulses_t *p, const wave_music_t *m, float dt, int *col)
{
	static float prof[BACK + FRONT + 1];
	if (!prof[BACK]) {
		for (int d = -BACK; d <= FRONT; d++) {
			float u = d > 0 ? d / 18.0f : -d / 60.0f;
			prof[d + BACK] = expf(-u * u);
		}
	}
	if (m->kicks != p->last) {
		p->last = m->kicks;
		int k = 0;
		for (int i = 1; i < WAVE_NPULSE; i++)
			if (p->a[i] < p->a[k])
				k = i;
		p->x[k] = -20;
		p->a[k] = m->kick;
	}
	memset(col, 0, sizeof(int) * WAVE_W);
	for (int i = 0; i < WAVE_NPULSE; i++) {
		if (p->a[i] < 0.02f)
			continue;
		p->x[i] += PV * dt;
		p->a[i] *= expf(-dt / 0.9f);
		if (p->x[i] > WAVE_W + BACK) {
			p->a[i] = 0;
			continue;
		}
		int a = (int)(p->a[i] * 256), hx = (int)p->x[i];
		for (int x = hx - BACK < 0 ? 0 : hx - BACK; x < WAVE_W && x <= hx + FRONT; x++)
			col[x] += (int)(a * prof[x - hx + BACK]);
	}
	for (int x = 0; x < WAVE_W; x++)
		col[x] = col[x] > 256 ? 256 : col[x];
}
