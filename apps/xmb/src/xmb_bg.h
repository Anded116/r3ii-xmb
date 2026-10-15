#ifndef XMB_BG_H
#define XMB_BG_H

#include <stdint.h>

#include "lvgl/lvgl.h"

/* стили фона — модули src/waves (порядок — как в xmb_bg.c и в меню) */
typedef enum {
	BG_WAVE_PS3,
	BG_WAVE_RIBBON,
	BG_WAVE_PARTICLES,
	BG_WAVE_SPECTRUM,
	BG_WAVE_SILK,
	BG_WAVE_AURORA,
	BG_WAVE_DUST,
	BG_WAVE_PULSE,
	BG_WAVE_RIPPLE,
	BG_WAVE_PSP,
	BG_WAVE_OFF,
	BG_WAVE_COUNT,
} bg_wave_t;

void bg_create(lv_obj_t *parent);
void bg_set_colors(uint32_t top, uint32_t bottom);
void bg_prerender(const uint32_t (*colors)[2], int n);   /* фоны тем — заранее, в фоне */
void bg_set_wave(bg_wave_t w);
void bg_tick(double dt);
void bg_set_level(double level);            /* 0..1 — уровень звука для визуализатора */
void bg_set_band(int player);               /* 1 — полоса волны под обложкой плеера */
void bg_set_dim(int opa);                   /* затемнение чёрным 0..255 — фон под раскрытым плеером */
void bg_set_spectrum(const float *v, int n); /* полосы спектра 0..1, от низких частот к высоким */
const lv_image_dsc_t *bg_glow(int size);    /* пятно свечения под выбранным элементом */

#endif
