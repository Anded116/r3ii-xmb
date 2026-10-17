/*
 * XMB на ПК: окно SDL 320×480.
 * ↑/↓ или колесо мыши — колесо плеера, Enter — клик колесом, Backspace/Esc — назад (тап),
 * ←/→ — свайп категорий, Space — Play/Pause, N — след. трек, P — питание, S — шторка. Мышь — тач.
 *
 * Без окна (проверка без рабочего стола): XMB_HEADLESS=1 XMB_SCRIPT="esc,up,enter,shot:a.ppm,..."
 * — сценарий выполняется по шагу раз в 300 мс, shot:<файл> сохраняет кадр в PPM, в конце выход.
 * Касание: drag:x0:y0:x1:y1[:мс] — палец от точки до точки (150 мс), tap:x:y — тап.
 */
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "filesave.h"
#include "xmb.h"

#define W 320
#define H 480

static uint16_t frame[W * H];
static char *script, *script_pos;
static struct { int on, x0, y0, x1, y1; uint32_t t0, ms; } drag;   /* касание из сценария */

static uint32_t tick_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int platform_battery(int *charging)
{
	*charging = 0;
	return 87;
}

int platform_battery_mv(void)
{
	return -1;
}

void platform_brightness(int percent)
{
	(void)percent;
}

static int key_watch(void *data, SDL_Event *e)
{
	(void)data;
	if (e->type == SDL_MOUSEWHEEL) {
		xmb_input(e->wheel.y > 0 ? XMB_WHEEL_UP : XMB_WHEEL_DOWN);
		return 0;
	}
	if (e->type != SDL_KEYDOWN)
		return 0;
	if (getenv("XMB_DEBUG"))
		printf("key %s\n", SDL_GetKeyName(e->key.keysym.sym));
	switch (e->key.keysym.sym) {
	case SDLK_UP:
	case SDLK_KP_8:      xmb_input(XMB_WHEEL_UP); break;
	case SDLK_DOWN:
	case SDLK_KP_2:      xmb_input(XMB_WHEEL_DOWN); break;
	case SDLK_RETURN:
	case SDLK_KP_ENTER:  xmb_input(XMB_SELECT); break;
	case SDLK_BACKSPACE:
	case SDLK_ESCAPE:    xmb_input(XMB_BACK); break;
	case SDLK_LEFT:
	case SDLK_KP_4:      xmb_input(XMB_CAT_PREV); break;
	case SDLK_RIGHT:
	case SDLK_KP_6:      xmb_input(XMB_CAT_NEXT); break;
	case SDLK_SPACE:     xmb_input(XMB_PLAY); break;
	case SDLK_n:         xmb_input(XMB_NEXT_TRACK); break;
	case SDLK_b:         xmb_input(XMB_PREV_TRACK); break;
	case SDLK_a:         xmb_input(XMB_NEXT_ALBUM); break;
	case SDLK_f:         xmb_input(XMB_FAV); break;
	case SDLK_m:         xmb_input(XMB_MENU); break;
	case SDLK_l:         xmb_input(XMB_ADD_PL); break;
	case SDLK_p:         xmb_input(XMB_POWER); break;
	case SDLK_s:         xmb_input(XMB_SHADE); break;
	}
	return 0;
}

/* ---------- режим без окна ---------- */

static void headless_flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
	(void)area; (void)px;
	lv_display_flush_ready(disp);
}

static void save_ppm(const char *path)
{
	lv_refr_now(NULL);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		return;
	}
	fprintf(f, "P6\n%d %d\n255\n", W, H);
	for (int i = 0; i < W * H; i++) {
		uint16_t p = frame[i];
		unsigned char rgb[3] = { (unsigned char)((p >> 11) << 3), (unsigned char)(((p >> 5) & 63) << 2), (unsigned char)((p & 31) << 3) };
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
	printf("shot %s\n", path);
}

/* палец идёт от начала к концу за drag.ms, ещё 50 мс лежит на месте и отпускается */
static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
	(void)indev;
	uint32_t t = tick_ms() - drag.t0;
	double f = !drag.ms || t >= drag.ms ? 1.0 : t / (double)drag.ms;
	data->point.x = (int32_t)(drag.x0 + (drag.x1 - drag.x0) * f);
	data->point.y = (int32_t)(drag.y0 + (drag.y1 - drag.y0) * f);
	data->state = drag.on && t < drag.ms + 50 ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
	if (data->state == LV_INDEV_STATE_RELEASED)
		drag.on = 0;
}

static void script_step(lv_timer_t *t)
{
	(void)t;
	if (!script_pos || !*script_pos) {
		fsave_flush();
		exit(0);
	}
	char *end = strchr(script_pos, ',');
	if (end)
		*end = 0;
	char *tok = script_pos;
	script_pos = end ? end + 1 : script_pos + strlen(script_pos);
	if (!strncmp(tok, "shot:", 5)) {
		save_ppm(tok + 5);
		return;
	}
	if (!strncmp(tok, "drag:", 5) || !strncmp(tok, "tap:", 4)) {
		int v[5] = { 0, 0, 0, 0, 150 };
		int n = sscanf(strchr(tok, ':') + 1, "%d:%d:%d:%d:%d", &v[0], &v[1], &v[2], &v[3], &v[4]);
		if (n == 2) {   /* тап */
			v[2] = v[0];
			v[3] = v[1];
			v[4] = 60;
		}
		drag.x0 = v[0], drag.y0 = v[1], drag.x1 = v[2], drag.y1 = v[3], drag.ms = (uint32_t)v[4];
		drag.t0 = tick_ms();
		drag.on = 1;
		return;
	}
	xmb_input_name(tok);
}

int main(int argc, char **argv)
{
	const char *zoom = getenv("XMB_ZOOM");
	lv_init();
	lv_tick_set_cb(tick_ms);
	if (getenv("XMB_HEADLESS")) {
		lv_display_t *disp = lv_display_create(W, H);
		lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
		lv_display_set_buffers(disp, frame, NULL, sizeof(frame), LV_DISPLAY_RENDER_MODE_DIRECT);
		lv_display_set_flush_cb(disp, headless_flush);
		lv_indev_t *touch = lv_indev_create();
		lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
		lv_indev_set_read_cb(touch, touch_read);
	} else {
		lv_display_t *disp = lv_sdl_window_create(W, H);
		lv_sdl_window_set_zoom(disp, zoom ? (float)atof(zoom) : 2.0f);
		lv_sdl_mouse_create();
		SDL_AddEventWatch(key_watch, NULL);
	}

	xmb_init(argc > 1 ? argv[1] : "res/fonts");

	if (getenv("XMB_SCRIPT")) {
		script = strdup(getenv("XMB_SCRIPT"));
		script_pos = script;
		lv_timer_create(script_step, getenv("XMB_STEP") ? atoi(getenv("XMB_STEP")) : 300, NULL);
	}

	while (1) {
		uint32_t wait = lv_timer_handler();
		usleep((wait > 1 && wait < 16 ? wait : 1) * 1000);
	}
	return 0;
}
