/*
 * wavetest — стенд модуля волны (src/waves/wave.h) без интерфейса: время кадра, площадь перерисовки,
 * проверка «вне dirty ничего не менялось», кадры в PPM.
 *
 *   gcc -O2 -Isrc/waves -DWAVE_SYM=wave_aurora tools/wavetest.c src/waves/wave_aurora.c -o /tmp/wt_aurora -lm
 *   /tmp/wt_aurora <папка для кадров>
 *
 * 900 кадров по 1/45 с: первые 450 — синтетическая музыка (бочка 2 раза в секунду, хэты, «вокал» в середине
 * спектра), дальше — тишина (пауза). На кадре 300 — смена фона (full = 1). Кадры 30, 120, 240, 420, 600 —
 * в <папка>/frame_NNN.ppm (полоса, вставленная в фон 320×480 на своё место) и все пять — в sheet.ppm.
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

int main(int argc, char **argv)
{
	const char *out = argc > 1 ? argv[1] : ".";
	const wave_t *w = &WAVE_SYM;
	srand(1);
	make_bg(0x5a3a08, 0xc98f2a);
	memcpy(dst, bg, sizeof(dst));
	w->init();
	float spec[WAVE_BANDS] = { 0 }, target[WAVE_BANDS];
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
		wave_frame_t fr = { t, 1 / 45.0, spec, level, i == 0 || i == 300 };
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
