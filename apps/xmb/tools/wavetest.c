/*
 * wavetest — стенд модуля волны (src/waves/wave.h) без интерфейса: время кадра, площадь перерисовки,
 * проверка «вне dirty ничего не менялось», кадры в PPM.
 *
 *   gcc -O2 -Isrc/waves -DWAVE_SYM=wave_aurora tools/wavetest.c src/waves/wave_aurora.c src/waves/wave_music.c -o /tmp/wt_aurora -lm
 *   /tmp/wt_aurora <папка для кадров>
 *
 * 900 кадров по 1/45 с: первые 450 — синтетическая музыка (бочка 2 раза в секунду, хэты, «вокал» в середине
 * спектра), дальше — тишина (пауза). На кадре 300 — смена фона (full = 1). Кадры 30, 120, 240, 420, 600 —
 * в <папка>/frame_NNN.ppm (полоса, вставленная в фон 320×480 на своё место) и все пять — в sheet.ppm.
 *
 * Настоящая музыка: WT_WAV=файл.wav (16 бит, стерео) — спектр и уровень считаются как в плеере (audio.c, xmb.c),
 * разбор — wave_music.c (собирать с src/waves/wave_music.c). WT_CSV=файл — признаки по кадрам, WT_ALL=1 — каждый
 * кадр в <папка>/f_NNNNN.ppm (полоса ±40 px) — видео: ffmpeg -i f_%05d.ppm -i файл.wav.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "wave.h"

#define STR2(x) #x
#define STR(x) STR2(x)
extern const wave_t WAVE_SYM;

#define SH 480
static uint16_t screen_bg[WAVE_W * SH];
static uint16_t bg[WAVE_W * WAVE_H], dst[WAVE_W * WAVE_H], prev[WAVE_W * WAVE_H];

static void make_bg(uint32_t top, uint32_t bottom)
{
	for (int y = 0; y < SH; y++) {
		double k = (double)y / (SH - 1);
		for (int x = 0; x < WAVE_W; x++) {
			int c[3];
			for (int i = 0; i < 3; i++) {
				int a = (top >> (16 - 8 * i)) & 255, b = (bottom >> (16 - 8 * i)) & 255;
				c[i] = (int)(a + (b - a) * k);
			}
			screen_bg[y * WAVE_W + x] = (uint16_t)(((c[0] >> 3) << 11) | ((c[1] >> 2) << 5) | (c[2] >> 3));
		}
	}
	memcpy(bg, screen_bg + WAVE_Y * WAVE_W, sizeof(bg));
}

static void rgb(uint16_t p, unsigned char *o)
{
	o[0] = (unsigned char)((p >> 11) << 3);
	o[1] = (unsigned char)(((p >> 5) & 63) << 2);
	o[2] = (unsigned char)((p & 31) << 3);
}

#define NSHOT 5
static const int shots[NSHOT] = { 30, 120, 240, 420, 600 };
static uint16_t shot_px[NSHOT][WAVE_W * SH];

static void save_ppm(const char *path, const uint16_t *px, int w, int h)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (int i = 0; i < w * h; i++) {
		unsigned char o[3];
		rgb(px[i], o);
		fwrite(o, 1, 3, f);
	}
	fclose(f);
}

static double now_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* спектр — как spectrum() в audio.c */
#define FFT_N 1024
static void spectrum(const int16_t *pcm, int rate, float *out)
{
	static float win[FFT_N], cs[FFT_N / 2], sn[FFT_N / 2], re[FFT_N], im[FFT_N];
	static int ready, lo[WAVE_BANDS], hi[WAVE_BANDS];
	if (!ready) {
		for (int i = 0; i < FFT_N; i++)
			win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_N - 1));
		for (int i = 0; i < FFT_N / 2; i++) {
			cs[i] = cosf(2 * (float)M_PI * i / FFT_N);
			sn[i] = -sinf(2 * (float)M_PI * i / FFT_N);
		}
		double fmax = rate / 2.0 < 16000 ? rate / 2.0 : 16000;
		for (int b = 0; b < WAVE_BANDS; b++) {
			double f0 = 40 * pow(fmax / 40, (double)b / WAVE_BANDS), f1 = 40 * pow(fmax / 40, (double)(b + 1) / WAVE_BANDS);
			lo[b] = (int)(f0 * FFT_N / rate);
			hi[b] = (int)(f1 * FFT_N / rate);
			lo[b] = lo[b] < 1 ? 1 : lo[b];
			hi[b] = hi[b] <= lo[b] ? lo[b] + 1 : hi[b] > FFT_N / 2 ? FFT_N / 2 : hi[b];
		}
		ready = 1;
	}
	for (unsigned i = 0; i < FFT_N; i++) {
		re[i] = (pcm[i * 2] + pcm[i * 2 + 1]) * 0.5f * 256 * win[i];
		im[i] = 0;
	}
	for (unsigned i = 1, j = 0; i < FFT_N; i++) {
		unsigned bit = FFT_N >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float x = re[i]; re[i] = re[j]; re[j] = x;
			x = im[i]; im[i] = im[j]; im[j] = x;
		}
	}
	for (unsigned len = 2; len <= FFT_N; len <<= 1) {
		unsigned step = FFT_N / len;
		for (unsigned i = 0; i < FFT_N; i += len)
			for (unsigned k = 0; k < len / 2; k++) {
				float wr = cs[k * step], wi = sn[k * step];
				unsigned p = i + k, q = p + len / 2;
				float xr = re[q] * wr - im[q] * wi, xi = re[q] * wi + im[q] * wr;
				re[q] = re[p] - xr;
				im[q] = im[p] - xi;
				re[p] += xr;
				im[p] += xi;
			}
	}
	const float ref = (FFT_N / 4.0f) * 8388608.0f;
	for (int b = 0; b < WAVE_BANDS; b++) {
		float pw = 0;
		for (int k = lo[b]; k < hi[b]; k++)
			pw += re[k] * re[k] + im[k] * im[k];
		float v = (10 * log10f(pw / (ref * ref) + 1e-12f) + 60) / 60;
		out[b] = v < 0 ? 0 : v > 1 ? 1 : v;
	}
}

static double approach(double v, double to, double dt, double rate)
{
	return v + (to - v) * fmin(1.0, dt * rate);
}

/* WAV: кадры по 1/45 с; кусок звука — 1024 кадра, как запись в плеере */
static int run_wav(const char *path, const char *out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return perror(path), 1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f) - 44;
	fseek(f, 44, SEEK_SET);
	int16_t *pcm = malloc(sz);
	if (fread(pcm, 1, sz, f) != (size_t)sz)
		return fprintf(stderr, "%s: короткий\n", path), 1;
	fclose(f);
	long nfr = sz / 4;
	const int rate = 44100;
	const wave_t *w = &WAVE_SYM;
	make_bg(0x5a3a08, 0xc98f2a);
	w->init();
	FILE *csv = getenv("WT_CSV") ? fopen(getenv("WT_CSV"), "w") : NULL;
	if (csv)
		fprintf(csv, "t,level,bass,mid,treb,bass_att,mid_att,treb_att,energy,kick,snare,hat,kicks,snares,hats\n");
	int all = getenv("WT_ALL") != NULL;
	float raw[WAVE_BANDS] = { 0 }, sp[WAVE_BANDS] = { 0 };
	double level = 0, sum_us = 0, max_us = 0;
	long chunk = -1;
	wave_music_t m = { 0 };
	int n = (int)(nfr * 45 / rate);
	for (int i = 0; i < n; i++) {
		double t = i / 45.0, dt = 1 / 45.0;
		long c = (long)(t * rate) / 1024 - 1;   /* последний доигравший кусок */
		if (c >= 0 && c != chunk) {
			chunk = c;
			spectrum(pcm + c * 1024 * 2, rate, raw);
		}
		double sum = 0;
		for (long k = c * 1024; k >= 0 && k < (c + 1) * 1024; k++)
			sum += (double)pcm[k * 2] * pcm[k * 2];
		double lv = c >= 0 ? fmin(1.0, sqrt(sum / 1024) / 32768 * 3.5) : 0;
		level = lv > level ? approach(level, lv, dt, 30) : approach(level, lv, dt, 5);
		for (int b = 0; b < WAVE_BANDS; b++)
			sp[b] = raw[b] > sp[b] ? sp[b] + (raw[b] - sp[b]) * (float)fmin(1.0, dt * 28) : sp[b] + (raw[b] - sp[b]) * (float)fmin(1.0, dt * 5);
		wave_music_update(&m, raw, (float)dt);
		wave_frame_t fr = { t, dt, sp, (float)level, i == 0, &m };
		wave_dirty_t d = { 0 };
		double t0 = now_us();
		w->render(dst, bg, &fr, &d);
		double us = now_us() - t0;
		sum_us += us;
		max_us = us > max_us ? us : max_us;
		if (csv)
			fprintf(csv, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%u,%u,%u\n", t, level, m.bass, m.mid,
				m.treb, m.bass_att, m.mid_att, m.treb_att, m.energy, m.kick, m.snare, m.hat, m.kicks, m.snares, m.hats);
		if (all) {
			static uint16_t crop[WAVE_W * (WAVE_H + 80)];
			memcpy(crop, screen_bg + (WAVE_Y - 40) * WAVE_W, sizeof(crop));
			memcpy(crop + 40 * WAVE_W, dst, sizeof(dst));
			char p[512];
			snprintf(p, sizeof(p), "%s/f_%05d.ppm", out, i);
			save_ppm(p, crop, WAVE_W, WAVE_H + 80);
		}
	}
	if (csv)
		fclose(csv);
	printf("%s: %d кадров, кадр в среднем %.1f мкс, макс %.1f мкс; ударов: бочка %u, малый %u, хэт %u\n", w->name, n,
	       sum_us / n, max_us, m.kicks, m.snares, m.hats);
	return 0;
}

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : ".";
	if (getenv("WT_WAV"))
		return run_wav(getenv("WT_WAV"), out);
	const wave_t *w = &WAVE_SYM;
	srand(1);
	make_bg(0x5a3a08, 0xc98f2a);
	memcpy(dst, bg, sizeof(dst));
	w->init();
	float spec[WAVE_BANDS] = { 0 }, target[WAVE_BANDS];
	wave_music_t mm = { 0 };
	double sum_us = 0, max_us = 0, sum_area = 0, music_area = 0, idle_area = 0;
	long bad = 0;
	int ns = 0;
	for (int i = 0; i < 900; i++) {
		double t = i / 45.0;
		int music = i < 450;
		double beat = fmod(t, 0.5), kick = music ? exp(-beat * 9) : 0;
		for (int b = 0; b < WAVE_BANDS; b++) {
			double v = 0;
			if (music) {
				v = b < 5 ? 0.25 + 0.7 * kick * (1 - b * 0.12) : 0;
				v += b >= 8 && b < 20 ? 0.35 + 0.25 * sin(t * 2.3 + b * 0.5) : 0;
				v += b >= 24 ? 0.2 + 0.3 * (rand() % 100) / 100.0 * exp(-fmod(t, 0.25) * 12) : 0;
			}
			target[b] = (float)(v > 1 ? 1 : v);
			spec[b] = target[b] > spec[b] ? spec[b] + (target[b] - spec[b]) * 0.6f : spec[b] + (target[b] - spec[b]) * 0.12f;
		}
		float level = music ? (float)(0.3 + 0.4 * kick) : 0;
		if (i == 300) {   /* смена темы */
			make_bg(0x0d2a5e, 0x4f86d8);
		}
		wave_music_update(&mm, target, 1 / 45.0f);
		wave_frame_t fr = { t, 1 / 45.0, spec, level, i == 0 || i == 300, &mm };
		wave_dirty_t d = { 0 };
		memcpy(prev, dst, sizeof(dst));
		double t0 = now_us();
		w->render(dst, bg, &fr, &d);
		double us = now_us() - t0;
		sum_us += us;
		max_us = us > max_us ? us : max_us;
		/* вне dirty — без изменений */
		static unsigned char cov[WAVE_W * WAVE_H];
		memset(cov, 0, sizeof(cov));
		long area = 0;
		for (int k = 0; k < d.n; k++) {
			wave_rect_t r = d.r[k];
			area += (long)(r.x2 - r.x1 + 1) * (r.y2 - r.y1 + 1);
			for (int y = r.y1; y <= r.y2; y++)
				memset(cov + y * WAVE_W + r.x1, 1, r.x2 - r.x1 + 1);
		}
		if (!fr.full)
			for (int p = 0; p < WAVE_W * WAVE_H; p++)
				if (!cov[p] && dst[p] != prev[p])
					bad++;
		double frac = (double)area / (WAVE_W * WAVE_H);
		if (!fr.full) {
			sum_area += frac;
			if (music)
				music_area += frac;
			else
				idle_area += frac;
		}
		if (ns < NSHOT && i == shots[ns]) {
			memcpy(shot_px[ns], screen_bg, sizeof(screen_bg));
			memcpy(shot_px[ns] + WAVE_Y * WAVE_W, dst, sizeof(dst));
			char p[512];
			snprintf(p, sizeof(p), "%s/frame_%03d.ppm", out, i);
			save_ppm(p, shot_px[ns], WAVE_W, SH);
			ns++;
		}
	}
	/* лист: пять кадров, полоса ±40 px — подряд сверху вниз */
	int crop0 = WAVE_Y - 40, ch = WAVE_H + 80;
	static uint16_t sheet[WAVE_W * (WAVE_H + 80) * NSHOT];
	for (int k = 0; k < NSHOT; k++)
		memcpy(sheet + k * WAVE_W * ch, shot_px[k] + crop0 * WAVE_W, sizeof(uint16_t) * WAVE_W * ch);
	char p[512];
	snprintf(p, sizeof(p), "%s/sheet.ppm", out);
	save_ppm(p, sheet, WAVE_W, ch * NSHOT);
	printf("%s: кадр в среднем %.1f мкс, макс %.1f мкс; перерисовка %.1f%% полосы (музыка %.1f%%, тишина %.1f%%); "
	       "изменено вне dirty: %ld пикс%s\n",
	       w->name, sum_us / 900, max_us, 100 * sum_area / 898, 100 * music_area / 449, 100 * idle_area / 449, bad,
	       bad ? " — ОШИБКА" : "");
	return bad ? 1 : 0;
}
