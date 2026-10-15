/*
 * fbtest — проверка фреймбуфера и ввода HiBy R3II 2025 без библиотек.
 * Мини-XMB: категории по горизонтали (кнопки prev/next), пункты по вертикали (колесо).
 * Play/Pause — сменить палитру, Power — выход.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W 320
#define H 480
#define NCAT 6
#define NITEM 12
#define NINPUT 5

static uint16_t back[W * H];
static uint16_t bg[W * H];

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static uint16_t rgb(int r, int g, int b)
{
	r = clamp(r, 0, 255);
	g = clamp(g, 0, 255);
	b = clamp(b, 0, 255);
	return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static const int palettes[][2][3] = {
	{{10, 40, 120}, {60, 140, 220}},   /* синий, как XMB по умолчанию */
	{{90, 10, 30}, {220, 70, 90}},     /* красный */
	{{10, 70, 30}, {80, 200, 110}},    /* зелёный */
	{{40, 20, 80}, {160, 110, 230}},   /* фиолетовый */
};
#define NPAL (int)(sizeof(palettes) / sizeof(palettes[0]))

static void make_bg(int pal)
{
	const int *a = palettes[pal][0], *b = palettes[pal][1];
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			/* вертикальный градиент + мягкая «волна» */
			double t = (double)y / H;
			double wave = 0.5 + 0.5 * sin(x * 0.02 + y * 0.004);
			double k = t * 0.8 + wave * 0.2;
			bg[y * W + x] = rgb(a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k, a[2] + (b[2] - a[2]) * k);
		}
	}
}

static void rect(int x0, int y0, int w, int h, uint16_t c)
{
	int x1 = x0 + w, y1 = y0 + h;
	x0 = clamp(x0, 0, W);
	y0 = clamp(y0, 0, H);
	x1 = clamp(x1, 0, W);
	y1 = clamp(y1, 0, H);
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			back[y * W + x] = c;
}

static void open_inputs(struct pollfd *pfd)
{
	char path[32];
	for (int i = 0; i < NINPUT; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		pfd[i].fd = open(path, O_RDONLY | O_NONBLOCK);
		pfd[i].events = POLLIN;
	}
}

int main(void)
{
	int fb = open("/dev/fb0", O_RDWR);
	if (fb < 0) { perror("fb0"); return 1; }
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	ioctl(fb, FBIOGET_VSCREENINFO, &vi);
	ioctl(fb, FBIOGET_FSCREENINFO, &fi);
	printf("fb %ux%u virt %ux%u bpp %u stride %u smem %u\n",
	       vi.xres, vi.yres, vi.xres_virtual, vi.yres_virtual, vi.bits_per_pixel, fi.line_length, fi.smem_len);
	uint8_t *mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }
	int pages = vi.yres_virtual / vi.yres;

	struct pollfd pfd[NINPUT];
	open_inputs(pfd);

	int cat = 0, item[NCAT] = {0}, pal = 0, running = 1, page = 0, frames = 0;
	double cat_pos = 0, item_pos = 0, t_fps = now();
	make_bg(pal);

	while (running) {
		poll(pfd, NINPUT, 16);
		for (int i = 0; i < NINPUT; i++) {
			struct input_event ev;
			while (pfd[i].fd >= 0 && read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY || ev.value != 1)
					continue;
				switch (ev.code) {
				case KEY_LEFT:         if (item[cat] < NITEM - 1) item[cat]++; break;  /* колесо по часовой */
				case KEY_RIGHT:        if (item[cat] > 0) item[cat]--; break;          /* колесо против */
				case KEY_NEXTSONG:     if (cat < NCAT - 1) cat++; break;
				case KEY_PREVIOUSSONG: if (cat > 0) cat--; break;
				case KEY_PLAYPAUSE:    pal = (pal + 1) % NPAL; make_bg(pal); break;
				case KEY_POWER:        running = 0; break;
				}
				printf("key %d -> cat %d item %d\n", ev.code, cat, item[cat]);
			}
		}

		/* плавное доведение к цели */
		cat_pos += (cat - cat_pos) * 0.25;
		item_pos += (item[cat] - item_pos) * 0.3;

		memcpy(back, bg, sizeof(back));

		/* ряд категорий: выбранная на x=60, шаг 90 */
		const int cy = 120;
		for (int c = 0; c < NCAT; c++) {
			double dx = (c - cat_pos) * 90;
			int sz = c == cat ? 56 : 40;
			int x = 60 + (int)dx - sz / 2, y = cy - sz / 2;
			uint16_t col = c == cat ? rgb(255, 255, 255) : rgb(200, 210, 230);
			rect(x, y, sz, sz, col);
		}

		/* вертикальный список выбранной категории под иконкой */
		for (int i = 0; i < NITEM; i++) {
			double dy = (i - item_pos) * 44;
			int y = (dy < 0 ? cy - 60 : cy + 70) + (int)dy;
			int on = i == item[cat];
			rect(40, y, on ? 36 : 28, on ? 36 : 28, on ? rgb(255, 255, 255) : rgb(170, 185, 215));
			rect(90, y + (on ? 12 : 10), on ? 190 : 150, on ? 12 : 8, on ? rgb(255, 255, 255) : rgb(150, 165, 200));
		}

		/* вывод: рисуем в неактивную страницу и переключаемся на неё */
		page = pages > 1 ? !page : 0;
		uint8_t *dst = mem + (size_t)page * vi.yres * fi.line_length;
		for (int y = 0; y < H; y++)
			memcpy(dst + y * fi.line_length, back + y * W, W * 2);
		vi.yoffset = page * vi.yres;
		ioctl(fb, FBIOPAN_DISPLAY, &vi);

		frames++;
		double t = now();
		if (t - t_fps >= 2.0) {
			printf("fps %.1f\n", frames / (t - t_fps));
			fflush(stdout);
			frames = 0;
			t_fps = t;
		}
	}

	munmap(mem, fi.smem_len);
	return 0;
}
