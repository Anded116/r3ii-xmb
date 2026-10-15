/* Образец модуля волны: одна тонкая синусоида. Каждый кадр — стереть прошлый след (восстановить фон)
 * и нарисовать новый; dirty — по столбцам шириной CHUNK: от минимума до максимума старого и нового следа. */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "wave.h"

#define CHUNK 16
#define THICK 2

static int prev_y[WAVE_W], have_prev;

static void init(void)
{
	have_prev = 0;
}

static void render(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty)
{
	int ny[WAVE_W];
	for (int x = 0; x < WAVE_W; x++) {
		double a = 2 * M_PI * x / WAVE_W;
		double amp = 18 + 30 * f->level;
		ny[x] = (int)(WAVE_H / 2 + amp * sin(a + f->t * 1.2) + 6 * sin(3 * a - f->t * 0.7));
	}
	if (f->full || !have_prev) {
		memcpy(dst, bg, sizeof(uint16_t) * WAVE_W * WAVE_H);
		wave_dirty_add(dirty, 0, 0, WAVE_W - 1, WAVE_H - 1);
	}
	for (int c = 0; c < WAVE_W; c += CHUNK) {
		int y1 = WAVE_H, y2 = -1;
		for (int x = c; x < c + CHUNK; x++) {
			if (have_prev && !f->full)   /* стереть старый след */
				for (int y = prev_y[x] - THICK; y <= prev_y[x] + THICK; y++)
					if (y >= 0 && y < WAVE_H)
						dst[y * WAVE_W + x] = bg[y * WAVE_W + x];
			int lo = ny[x] - THICK, hi = ny[x] + THICK;
			for (int y = lo; y <= hi; y++)
				if (y >= 0 && y < WAVE_H) {
					int d = abs(y - ny[x]);
					dst[y * WAVE_W + x] = wave_lighten(bg[y * WAVE_W + x], d == 0 ? 150 : d == 1 ? 80 : 25);
				}
			int a = have_prev ? (prev_y[x] < ny[x] ? prev_y[x] : ny[x]) : ny[x];
			int b = have_prev ? (prev_y[x] > ny[x] ? prev_y[x] : ny[x]) : ny[x];
			y1 = a - THICK < y1 ? a - THICK : y1;
			y2 = b + THICK > y2 ? b + THICK : y2;
		}
		if (!f->full && have_prev)
			wave_dirty_add(dirty, c, y1, c + CHUNK - 1, y2);
	}
	memcpy(prev_y, ny, sizeof(ny));
	have_prev = 1;
}

const wave_t wave_line = { "Линия", init, render };
