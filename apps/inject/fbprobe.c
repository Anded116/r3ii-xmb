/*
 * fbprobe — после FBIOBLANK вывод кадров уходит в таймаут: перебрать способы разбудить панель
 * и после каждого замерить время FBIOPAN_DISPLAY (норма ~22 мс, сломано ~500 мс).
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static int fd;
static struct fb_var_screeninfo vi;

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static void pans(const char *what)
{
	double worst = 0, sum = 0;
	for (int i = 0; i < 6; i++) {
		vi.yoffset = (i & 1) * vi.yres;
		double t = now();
		int r = ioctl(fd, FBIOPAN_DISPLAY, &vi);
		double d = now() - t;
		sum += d;
		if (d > worst)
			worst = d;
		if (r < 0)
			perror("pan");
	}
	printf("%-34s pan avg %6.1f ms, max %6.1f ms\n", what, sum / 6, worst);
	fflush(stdout);
}

static void blank(int v)
{
	double t = now();
	int r = ioctl(fd, FBIOBLANK, v);
	printf("  FBIOBLANK %d -> %d (%.0f ms)\n", v, r, now() - t);
}

int main(void)
{
	fd = open("/dev/fb0", O_RDWR);
	ioctl(fd, FBIOGET_VSCREENINFO, &vi);
	struct fb_fix_screeninfo fi;
	ioctl(fd, FBIOGET_FSCREENINFO, &fi);
	uint16_t *mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	for (unsigned i = 0; i < fi.smem_len / 2; i++)
		mem[i] = (i / 320) % 40 < 20 ? 0xffff : 0x001f;   /* полосы, чтобы было видно глазом */

	pans("0. как есть");
	blank(FB_BLANK_UNBLANK);
	pans("1. UNBLANK");
	blank(FB_BLANK_POWERDOWN);
	usleep(300000);
	blank(FB_BLANK_UNBLANK);
	pans("2. POWERDOWN -> UNBLANK");
	blank(FB_BLANK_NORMAL);
	blank(FB_BLANK_UNBLANK);
	pans("3. NORMAL -> UNBLANK");
	vi.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	vi.yoffset = 0;
	printf("  FBIOPUT_VSCREENINFO -> %d\n", ioctl(fd, FBIOPUT_VSCREENINFO, &vi));
	vi.activate = 0;
	pans("4. PUT_VSCREENINFO");
	close(fd);
	fd = open("/dev/fb0", O_RDWR);
	pans("5. reopen fb0");
	return 0;
}
