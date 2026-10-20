#include "blackbox.h"

#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#define BB_PATH "/data/mnt/sd_0/hiby_dev/bb.log"
#define BB_MAX (256 * 1024)

static volatile int bb_on = 1;

void bb_set(int on)
{
	bb_on = on;
}

void bb(const char *fmt, ...)
{
#if XMB_DEVICE
	if (!bb_on)
		return;
	char line[192];
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	int n = snprintf(line, sizeof(line), "%02d:%02d:%02d %6ld.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec,
			 (long)ts.tv_sec, ts.tv_nsec / 1000000);
	va_list ap;
	va_start(ap, fmt);
	n += vsnprintf(line + n, sizeof(line) - n - 1, fmt, ap);
	va_end(ap);
	if (n > (int)sizeof(line) - 2)
		n = sizeof(line) - 2;
	line[n++] = '\n';
	/* открыть на строку и закрыть: открытый файл держал бы карту (режим накопителя); нет карты — тихо мимо */
	int fd = open(BB_PATH, O_WRONLY | O_APPEND | O_CREAT | O_SYNC, 0644);
	if (fd < 0)
		return;
	if (lseek(fd, 0, SEEK_END) > BB_MAX)
		if (ftruncate(fd, 0)) {
		}
	if (write(fd, line, n) < 0)
		perror("bb");
	close(fd);
#else
	(void)fmt;
#endif
}
