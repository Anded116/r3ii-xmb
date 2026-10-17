/*
 * fbshow <файл> — вывести кадр RGB565 320×480 на экран (на обе страницы fb, pan на первую). XMB — остановить.
 * Нужен для проверочных картинок, например сколько пикселей у края закрывает рамка корпуса.
 */
#include <fcntl.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: fbshow <rgb565 320x480>\n");
		return 1;
	}
	static uint16_t img[320 * 480];
	FILE *f = fopen(argv[1], "rb");
	if (!f || fread(img, 2, 320 * 480, f) != 320 * 480) {
		perror(argv[1]);
		return 1;
	}
	fclose(f);
	int fd = open("/dev/fb0", O_RDWR);
	struct fb_var_screeninfo vi;
	struct fb_fix_screeninfo fi;
	if (fd < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vi) || ioctl(fd, FBIOGET_FSCREENINFO, &fi)) {
		perror("fb0");
		return 1;
	}
	uint8_t *mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (mem == MAP_FAILED) {
		perror("mmap");
		return 1;
	}
	for (unsigned page = 0; page * vi.yres * fi.line_length + 480 * fi.line_length <= fi.smem_len && page < 2; page++)
		for (int y = 0; y < 480; y++)
			memcpy(mem + (page * vi.yres + y) * fi.line_length, img + y * 320, 320 * 2);
	vi.yoffset = 0;
	ioctl(fd, FBIOPAN_DISPLAY, &vi);
	return 0;
}
