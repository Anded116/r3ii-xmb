/*
 * touchtest — замер тача HiBy R3II 2025 и ощущения прокрутки пальцем.
 * Слева список из 40 строк: тащить пальцем, бросок — инерция, колесо — по строке.
 * Белый квадрат — сырая позиция пальца (видно отставание отрисовки).
 * В stdout по каждому касанию: длительность, число отчётов, частота, задержка доставки события.
 * Play/Pause — сырой режим / предсказание позиции пальца (индикатор в углу: красный / зелёный).
 * Power или «пред. трек» — выход.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define W 320
#define H 480
#define NINPUT 5
#define ROWS 40
#define ROW_H 56

static uint16_t back[W * H];

static int clamp(int v, int lo, int hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static uint16_t rgb(int r, int g, int b)
{
	return (uint16_t)(((clamp(r, 0, 255) >> 3) << 11) | ((clamp(g, 0, 255) >> 2) << 5) | (clamp(b, 0, 255) >> 3));
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void rect(int x0, int y0, int w, int h, uint16_t c)
{
	int x1 = clamp(x0 + w, 0, W), y1 = clamp(y0 + h, 0, H);
	x0 = clamp(x0, 0, W);
	y0 = clamp(y0, 0, H);
	for (int y = y0; y < y1; y++)
		for (int x = x0; x < x1; x++)
			back[y * W + x] = c;
}

int main(void)
{
	int fb = open("/dev/fb0", O_RDWR);
	if (fb < 0) { perror("fb0"); return 1; }
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	ioctl(fb, FBIOGET_VSCREENINFO, &vi);
	ioctl(fb, FBIOGET_FSCREENINFO, &fi);
	uint8_t *mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (mem == MAP_FAILED) { perror("mmap"); return 1; }
	int pages = vi.yres_virtual / vi.yres, page = 0;

	struct pollfd pfd[NINPUT];
	char path[32];
	for (int i = 0; i < NINPUT; i++) {
		snprintf(path, sizeof(path), "/dev/input/event%d", i);
		pfd[i].fd = open(path, O_RDONLY | O_NONBLOCK);
		pfd[i].events = POLLIN;
		int clk = CLOCK_MONOTONIC;
		if (pfd[i].fd >= 0)
			ioctl(pfd[i].fd, EVIOCSCLOCKID, &clk);
	}

	const double max_scroll = ROWS * ROW_H - H;
	double scroll = 0, vel = 0, wheel_target = 0;
	int wheel_active = 0;
	int touching = 0, tx = -1, ty = -1, raw_x = 0, raw_y = 0, last_y = 0;
	double last_t = 0, t_down = 0, lat_sum = 0, lat_max = 0;
	int reports = 0, running = 1, frames = 0, smooth = 1;
	double scroll_down = 0, down_y = 0, fy = 0, fvel = 0, ft = 0;
	double t_fps = now(), t_prev = now();

	while (running) {
		poll(pfd, NINPUT, 5);
		for (int i = 0; i < NINPUT; i++) {
			struct input_event ev;
			while (pfd[i].fd >= 0 && read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				double et = ev.input_event_sec + ev.input_event_usec / 1e6;
				if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_X) raw_x = ev.value;
				if (ev.type == EV_ABS && ev.code == ABS_MT_POSITION_Y) raw_y = ev.value;
				if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
					if (ev.value) {
						touching = 1; reports = 0; lat_sum = lat_max = 0; t_down = et;
						vel = 0; wheel_active = 0; last_y = -1; scroll_down = scroll; fvel = 0;
					} else {
						double dur = et - t_down;
						printf("[%s] touch: %.0f ms, %d reports, %.0f Hz, delivery avg %.1f ms max %.1f ms, release vel %.0f px/s\n",
						       smooth ? "smooth" : "raw", dur * 1000, reports, dur > 0 ? (reports - 1) / dur : 0,
						       reports ? lat_sum / reports * 1000 : 0, lat_max * 1000, vel);
						fflush(stdout);
						touching = 0;
					}
				}
				if (ev.type == EV_SYN && ev.code == SYN_REPORT && touching) {
					double lat = now() - et;
					lat_sum += lat;
					if (lat > lat_max) lat_max = lat;
					reports++;
					tx = raw_x; ty = raw_y;
					if (last_y < 0) {
						down_y = ty;
					} else {
						double dt = et - last_t;
						if (dt > 0) {
							vel = vel * 0.6 + (-(ty - last_y) / dt) * 0.4;
							fvel = fvel * 0.5 + ((ty - last_y) / dt) * 0.5;
						}
					}
					fy = ty; ft = et;
					if (!smooth)
						scroll = scroll_down - (ty - down_y);
					last_y = ty; last_t = et;
				}
				if (ev.type == EV_KEY && ev.value == 1) {
					double base = wheel_active ? wheel_target : round(scroll / ROW_H) * ROW_H;
					if (ev.code == KEY_LEFT || ev.code == KEY_RIGHT) {
						wheel_target = fmin(fmax(base + (ev.code == KEY_LEFT ? ROW_H : -ROW_H), 0), max_scroll);
						wheel_active = 1;
						vel = 0;
					}
					if (ev.code == KEY_POWER || ev.code == KEY_PREVIOUSSONG) running = 0;
					if (ev.code == KEY_PLAYPAUSE) smooth = !smooth;
				}
			}
		}

		double t = now(), dt = t - t_prev;
		t_prev = t;
		if (touching && smooth && last_y >= 0) {
			/* экстраполяция позиции пальца на момент кадра, не дальше одного интервала тача */
			double ahead = fmin(t - ft, 0.035);
			scroll = scroll_down - (fy + fvel * ahead - down_y);
		}
		if (!touching) {
			if (wheel_active) {
				scroll += (wheel_target - scroll) * 0.35;
				if (fabs(wheel_target - scroll) < 0.5) { scroll = wheel_target; wheel_active = 0; }
			} else if (fabs(vel) > 5) {
				scroll += vel * dt;
				vel *= pow(0.05, dt);  /* затухание инерции: −95% скорости за секунду */
			}
		}
		if (scroll < 0) { scroll = 0; vel = 0; }
		if (scroll > max_scroll) { scroll = max_scroll; vel = 0; }

		/* список */
		rect(0, 0, W, H, rgb(12, 30, 70));
		int first = (int)(scroll / ROW_H);
		for (int r = first; r < ROWS && r <= first + H / ROW_H + 1; r++) {
			int y = r * ROW_H - (int)scroll;
			int shade = r % 2 ? 0 : 18;
			rect(0, y, 240, ROW_H - 2, rgb(40 + shade, 80 + shade, 150 + shade));
			rect(12, y + 10, 34, 34, rgb(230, 235, 245));
			rect(58, y + 16, 40 + (r * 37) % 120, 10, rgb(220, 228, 240));
			rect(58, y + 32, 30 + (r * 53) % 90, 6, rgb(150, 170, 210));
		}
		/* полоса прокрутки */
		int bar_h = H * H / (ROWS * ROW_H);
		rect(244, (int)(scroll / (ROWS * ROW_H) * H), 4, bar_h, rgb(255, 255, 255));
		rect(W - 24, 8, 16, 16, smooth ? rgb(40, 220, 90) : rgb(230, 50, 50));
		/* сырая позиция пальца */
		if (touching && tx >= 0)
			rect(tx - 12, ty - 12, 24, 24, rgb(255, 255, 255));

		page = pages > 1 ? !page : 0;
		uint8_t *dst = mem + (size_t)page * vi.yres * fi.line_length;
		for (int y = 0; y < H; y++)
			memcpy(dst + y * fi.line_length, back + y * W, W * 2);
		vi.yoffset = page * vi.yres;
		ioctl(fb, FBIOPAN_DISPLAY, &vi);

		frames++;
		if (t - t_fps >= 5.0) {
			printf("fps %.1f\n", frames / (t - t_fps));
			fflush(stdout);
			frames = 0;
			t_fps = t;
		}
	}
	munmap(mem, fi.smem_len);
	return 0;
}
