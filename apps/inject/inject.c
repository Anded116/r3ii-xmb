/* inject <dev> <code> <value> [<code> <value>...] — записать EV_KEY + SYN в evdev-устройство */
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

int main(int argc, char **argv)
{
	if (argc < 4) {
		fprintf(stderr, "usage: inject <dev> <code> <value>...\n");
		return 1;
	}
	int fd = open(argv[1], O_WRONLY);
	if (fd < 0) { perror(argv[1]); return 1; }
	for (int i = 2; i + 1 < argc; i += 2) {
		put(fd, EV_KEY, atoi(argv[i]), atoi(argv[i + 1]));
		put(fd, EV_SYN, SYN_REPORT, 0);
	}
	close(fd);
	return 0;
}
