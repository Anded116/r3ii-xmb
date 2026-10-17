/* inject <dev> <code> <value> [<code> <value>...] — записать EV_KEY + SYN в evdev-устройство
 * inject <dev> drag <x0> <y0> <x1> <y1> [мс] — палец по тачу (мультитач goodix, 33 Гц, как настоящий) */
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void put(int fd, int type, int code, int value)
{
	struct input_event ev;
	memset(&ev, 0, sizeof(ev));
	ev.type = type;
	ev.code = code;
	ev.value = value;
	if (write(fd, &ev, sizeof(ev)) != sizeof(ev))
		perror("write");
}

static void touch(int fd, int x, int y, int down)
{
	if (down) {
		put(fd, EV_ABS, ABS_MT_TRACKING_ID, 1);
		put(fd, EV_ABS, ABS_MT_POSITION_X, x);
		put(fd, EV_ABS, ABS_MT_POSITION_Y, y);
		put(fd, EV_ABS, ABS_MT_TOUCH_MAJOR, 20);
	} else {
		put(fd, EV_ABS, ABS_MT_TRACKING_ID, -1);
	}
	put(fd, EV_KEY, BTN_TOUCH, down);
	put(fd, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: inject <dev> <code> <value>...\n");
		return 1;
	}
	int fd = open(argv[1], O_WRONLY);
	if (fd < 0) { perror(argv[1]); return 1; }
	if (!strcmp(argv[2], "drag") && argc >= 7) {
		int x0 = atoi(argv[3]), y0 = atoi(argv[4]), x1 = atoi(argv[5]), y1 = atoi(argv[6]);
		int ms = argc > 7 ? atoi(argv[7]) : 200, n = ms / 30 > 1 ? ms / 30 : 1;
		for (int i = 0; i <= n; i++) {
			touch(fd, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, 1);
			usleep(30000);
		}
		touch(fd, x1, y1, 0);
		close(fd);
		return 0;
	}
	for (int i = 2; i + 1 < argc; i += 2) {
		put(fd, EV_KEY, atoi(argv[i]), atoi(argv[i + 1]));
		put(fd, EV_SYN, SYN_REPORT, 0);
	}
	close(fd);
	return 0;
}
