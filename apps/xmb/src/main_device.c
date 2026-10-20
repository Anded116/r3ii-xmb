/*
 * XMB на HiBy R3II 2025: fb0 (RGB565, две страницы, FBIOPAN_DISPLAY), ввод evdev.
 *
 * event3 колесо: KEY_LEFT (по часовой) — вниз, KEY_RIGHT — вверх.
 * event0: KEY_POWER (клик колесом) — выбор, удержание — меню и сон, будит из сна; KEY_PREVIOUSSONG — «след. трек».
 * event2: KEY_NEXTSONG — кнопка «пред. трек» на корпусе: питание экрана;
 *         KEY_PLAYPAUSE — пауза. (Коды кнопок треков в драйвере перепутаны относительно корпуса.)
 * event1: тач через lv_evdev.
 */
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <time.h>
#include <unistd.h>

#include "lvgl/lvgl.h"
#include "audio.h"
#include "blackbox.h"
#include "subproc.h"
#include "filesave.h"
#include "xmb.h"

#define W 320
#define H 480
#define HOLD_PREV_MS 600
#define DOUBLE_MS 300   /* окно второго нажатия */

/* жесты кнопки: короткое (при отпускании; если назначено двойное — после окна второго нажатия),
 * долгое (по удержанию, сразу), двойное */
typedef struct {
	int button, has_long;
	uint32_t down, up;
	int held, long_fired, pending, second;
} btn_t;

static void btn_event(btn_t *b, int value, uint32_t now)
{
	if (value == 1) {
		if (b->pending && now - b->up <= DOUBLE_MS)
			b->second = 1;
		b->pending = 0;
		b->held = 1;
		b->long_fired = 0;
		b->down = now;
	} else if (value == 0 && b->held) {
		b->held = 0;
		if (b->long_fired) {
			b->second = 0;
		} else if (b->second) {
			b->second = 0;
			xmb_button(b->button, XMB_G_DOUBLE);
		} else if (xmb_button_has_double(b->button)) {
			b->pending = 1;
			b->up = now;
		} else {
			xmb_button(b->button, XMB_G_SHORT);
		}
	}
}

static void btn_tick(btn_t *b, uint32_t now)
{
	if (b->held && b->has_long && !b->long_fired && !b->second && now - b->down >= HOLD_PREV_MS) {
		b->long_fired = 1;
		xmb_button(b->button, XMB_G_LONG);
	}
	if (b->pending && now - b->up > DOUBLE_MS) {
		b->pending = 0;
		xmb_button(b->button, XMB_G_SHORT);
	}
}
#define BACKLIGHT "/sys/class/backlight/backlight_pwm0/brightness"

static int fb_fd;
static uint8_t *fb_mem;
static struct fb_var_screeninfo vi;
static struct fb_fix_screeninfo fi;
static int fb_page;
static uint16_t frame[W * H];
static volatile sig_atomic_t shot_req;   /* kill -USR1: текущий кадр — в /tmp/xmb_shot.raw (RGB565 320×480) */

static void shot_sig(int sig)
{
	(void)sig;
	shot_req = 1;
}
static int frames;

#define SLEEP_HOLD_MS 2000    /* удержание колеса (кнопка питания): с HOLD_PREV_MS — меню, отсюда — отсчёт до сна */
#define SLEEP_COUNT_MS 1500   /* отсчёт «3, 2, 1» по полсекунды */
#define AUTO_COUNT_MS 3000    /* перед автоматическим сном: экран с отсчётом 3 с */

static int fb_reprogram;
static void screen_set(int on);
static uint32_t tick_ms(void);
static uint32_t woke_ms;   /* когда вернулась запись в /sys/power/state: время до включённого экрана — в журнал */
static uint32_t rem_quiet;   /* до этого времени кнопки пультов не слушать: проводной после сна шлёт фантомное нажатие */

/* число прерываний кнопки (строка /proc/interrupts с её именем): кто разбудил */
static unsigned long irq_count(const char *name)
{
	char line[256];
	unsigned long n = 0;
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f)
		return 0;
	while (fgets(line, sizeof(line), f))
		if (strstr(line, name)) {
			n = strtoul(strchr(line, ':') ? strchr(line, ':') + 1 : line, NULL, 10);
			break;
		}
	fclose(f);
	return n;
}

/* счётчики прерываний по номерам (до 128) — кто разбудил: что прибавилось за сон */
#define NIRQ 128
static void irq_snap(unsigned long *n)
{
	char line[256];
	memset(n, 0, sizeof(unsigned long) * NIRQ);
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *e;
		long i = strtol(line, &e, 10);
		if (e != line && *e == ':' && i >= 0 && i < NIRQ)
			n[i] = strtoul(e + 1, NULL, 10);
	}
	fclose(f);
}

/* прибавившиеся за сон прерывания с именами, кроме таймера и каскада: «KEY_POWER+1 rtc 1Hz and alarm+1» */
static void irq_woke(const unsigned long *a, char *out, int cap)
{
	char line[256];
	int len = 0;
	out[0] = 0;
	FILE *f = fopen("/proc/interrupts", "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *e;
		long i = strtol(line, &e, 10);
		if (e == line || *e != ':' || i < 0 || i >= NIRQ || strstr(line, "timerost") || strstr(line, "cascade"))
			continue;
		unsigned long n;
		int pos = 0;
		if (sscanf(e + 1, "%lu %*s %*d %n", &n, &pos) < 1 || n == a[i])
			continue;
		char *name = e + 1 + pos;   /* «   97:  11  GPIO  31  KEY_POWER»: счётчик, контроллер, линия, имя */
		name[strcspn(name, "\n")] = 0;
		len += snprintf(out + len, len < cap ? cap - len : 0, "%s%s+%lu", len ? ", " : "", *name ? name : "?", n - a[i]);
		if (len >= cap)
			break;
	}
	fclose(f);
}

/* глубокий сон (suspend to RAM): пауза, экран гаснет. Железо будит клик колесом и «след. трек» (md-gpio-keys),
 * кнопки на АЦП — нет. Будит только колесо, как кнопка питания: от «след. трек» (карман) — сразу обратно в сон.
 * Запись в /sys/power/state возвращается после сна */
static int usb_host(void);
static int key_fd = -1;   /* event0: колесо */

static int wheel_held(void)
{
	unsigned char k[KEY_MAX / 8 + 1] = { 0 };
	return key_fd >= 0 && ioctl(key_fd, EVIOCGKEY(sizeof(k)), k) >= 0 && (k[KEY_POWER / 8] >> (KEY_POWER % 8) & 1);
}

static int mem_blocked;   /* Wi-Fi завис: сна до перезагрузки нет — и автосон не зажигает экран отсчётом */

static int deep_sleep(void)
{
	/* XMB_FORCE_MEM — проверка сна на кабеле (сценарий mem): иначе компьютер разбудил бы сразу */
	int slept = 0, host = usb_host() && !getenv("XMB_FORCE_MEM");
	screen_set(0);   /* сразу, как сток: выключение радио и звука — секунды, уже в темноте */
	int radio = xmb_sleep_prepare(!host);
	/* компьютер на кабеле будит сразу же, а переподключение USB после пробуждения рвёт adb — только гасим экран */
	if (host) {
		printf("на кабеле компьютера — без сна, экран погашен\n");
		return 1;
	}
	if (radio < 0) {   /* сон с невыключенным радио вешает плеер насовсем: только экран */
		bb("сон: без mem — радио %s", radio == -1 ? "зависло, до перезагрузки" : "не выключилось");
		if (radio == -1)
			mem_blocked = 1;
		return 0;
	}
	audio_power_down();
	sync();
	/* отсчёт идёт под удержанием колеса: отпускание во сне тут же будит — сначала дождаться его */
	for (int i = 0; i < 100 && wheel_held(); i++)
		usleep(100000);
	for (int i = 0; i < 100; i++) {
		unsigned long wheel = irq_count("KEY_POWER"), next = irq_count("KEY_PREVIOUSSONG"), irq0[NIRQ];
		irq_snap(irq0);
		printf("suspend\n");
		fflush(stdout);
		bb("сон: mem");
		int fd = open("/sys/power/state", O_WRONLY);
		if (fd >= 0) {
			if (write(fd, "mem", 3) < 0)
				perror("suspend");
			else
				slept = 1;
			close(fd);
		}
		char woke[160];
		irq_woke(irq0, woke, sizeof(woke));
		bb("сон: разбудили — %s", woke);
		if (!slept)
			break;
		if (irq_count("KEY_POWER") != wheel || irq_count("KEY_PREVIOUSSONG") == next)
			break;   /* колесо или не кнопка (кабель, зарядка) — просыпаемся */
		printf("woken by next-track, back to sleep\n");
		usleep(300000);   /* дать кнопке отпуститься */
	}
	woke_ms = slept ? tick_ms() : 0;
	if (slept)
		rem_quiet = tick_ms() + 1000;
	bb("сон: проснулся (%s)", slept ? "был сон" : "сна не было");
	xmb_sleep_end();
	if (!slept) {   /* ядро отказало (кто-то держит блокировку сна): экран не трогать — сна не было */
		printf("suspend не удался — экран остаётся погашенным, следующая попытка через 5 мин\n");
		return 0;
	}
	fb_reprogram = 1;
	xmb_wake();
	printf("resume\n");
	if (sp_system("setsid sh /tmp/adb_restart.sh > /dev/null 2>&1 < /dev/null &") != 0)   /* adbd после сна — заново */
		perror("adb_restart");
	return 1;
}

/* ---------- USB-накопитель ----------
 * Стоковый гаджет android0 (mass_storage.0) уже собран системой при загрузке; контроллер один — его держит либо он
 * (обычно: пустой картридер, по нему видно, что подключён компьютер), либо наш adb_demo (отладка). Режим накопителя:
 * всё сохранено и закрыто, карта отмонтирована, android0 с картой привязан к контроллеру. Конец — кабель вынут,
 * компьютер извлёк диск или удержание колеса: карта снова плееру, контроллер — прежнему гаджету, XMB перезапускается
 * (код 3 — run_xmb.sh запускает его снова) и перечитывает библиотеку. Так же — у Sonix */
#define GADGETS "/sys/kernel/config/usb_gadget"
#define LUN GADGETS "/android0/functions/mass_storage.0/lun.0/file"
#define SD_DEV "/dev/mmcblk0p1"
#define SD_MNT "/usr/data/mnt/sd_0"
#define SD_OPTS "fmask=0022,dmask=0022,iocharset=utf8,namecase=0"
#define XMB_RESTART 3
#define XMB_POWER_OFF 4
#define USB_HOLD_MS 1000   /* в режиме накопителя: удержание колеса — вернуть карту */

static int usb_storage, usb_adb;   /* режим накопителя; до него контроллер держал adb_demo */
static int usb_dac, dac_fd = -1;   /* режим USB-ЦАПа и его /dev/uac_sa */
static char dac_ms_link[64];       /* функция накопителя в конфигурации — вернуть после ЦАПа */
static char dac_class[3][16];      /* класс устройства android0 до ЦАПа */
static char usb_udc[64];

static int read_line(const char *path, char *buf, int n)
{
	FILE *f = fopen(path, "r");
	buf[0] = 0;
	if (!f)
		return 0;
	if (fgets(buf, n, f))
		buf[strcspn(buf, "\n")] = 0;
	fclose(f);
	return buf[0] != 0;
}

static void write_str(const char *path, const char *s)
{
	FILE *f = fopen(path, "w");
	if (!f)
		return;
	fputs(s, f);
	fputs("\n", f);
	fclose(f);
}

static int usb_power(void)
{
	char b[32];
	return read_line("/sys/class/power_supply/usb/online", b, sizeof(b)) && !strcmp(b, "1");
}

/* компьютер на том конце: USB-питание есть и контроллер прошёл перечисление (зарядка его не проходит) */
static int usb_host(void)
{
	char b[32], path[128];
	if (!usb_power())
		return 0;
	snprintf(path, sizeof(path), "/sys/class/udc/%s/state", usb_udc);
	return read_line(path, b, sizeof(b)) && !strcmp(b, "configured");
}

/* 0 — карту не удалось отмонтировать (что-то держит файл): компьютеру её не отдаём */
static int usb_storage_begin(void)
{
	char b[64];
	xmb_storage_prepare();
	fflush(stdout);
	fflush(stderr);
	int fd = open("/tmp/xmb_usb.log", O_WRONLY | O_CREAT | O_APPEND, 0644);   /* журнал на карте держит её */
	if (fd >= 0) {
		dup2(fd, 1);
		dup2(fd, 2);
		close(fd);
	}
	sync();
	int ok = 0;
	for (int i = 0; i < 10 && !ok; i++) {
		ok = umount(SD_MNT) == 0;
		if (!ok)
			usleep(300000);
	}
	if (!ok) {   /* отдать карту, в которую ещё пишут, — испортить файлы */
		perror("usb: umount");
		return 0;
	}
	printf("usb: storage\n");
	usb_adb = read_line(GADGETS "/adb_demo/UDC", b, sizeof(b));
	if (usb_adb)
		write_str(GADGETS "/adb_demo/UDC", "");
	write_str(GADGETS "/android0/UDC", "");
	write_str(LUN, SD_DEV);
	write_str(GADGETS "/android0/UDC", usb_udc);
	usb_storage = 1;
	return 1;
}

static int sd_mounted(void)
{
	FILE *f = fopen("/proc/mounts", "r");
	char line[256];
	int found = 0;
	while (f && !found && fgets(line, sizeof(line), f))
		found = strstr(line, " " SD_MNT " ") != NULL;
	if (f)
		fclose(f);
	return found;
}

static void usb_storage_end(void)
{
	write_str(LUN, "");
	write_str(GADGETS "/android0/UDC", "");
	if (mount(SD_DEV, SD_MNT, "exfat", MS_RELATIME, SD_OPTS) < 0)
		perror("usb: mount");
	write_str(usb_adb ? GADGETS "/adb_demo/UDC" : GADGETS "/android0/UDC", usb_udc);
	printf("usb: storage end\n");
	fflush(stdout);
}

/* ---------- USB-ЦАП ----------
 * Как у стока (uac_device_config.sh и hiby_player, разобрано в Sonix): в гаджете android0 вместо накопителя —
 * функция uac_sa, вшитая в ядро; она создаёт /dev/uac_sa, из него поток звука и читает (audio.c). Карта остаётся
 * у плеера. adb в одном гаджете с ЦАПом Windows не принимает — на время режима его нет */
#define A0 GADGETS "/android0"
#define UAC A0 "/functions/uac_sa.a"
static const char *const dac_class_attr[3] = { A0 "/bDeviceClass", A0 "/bDeviceSubClass", A0 "/bDeviceProtocol" };

static int usb_dac_begin(void)
{
	char b[64];
	printf("usb: ЦАП\n");
	fflush(stdout);
	xmb_dac_prepare();
	usb_adb = read_line(GADGETS "/adb_demo/UDC", b, sizeof(b));
	if (usb_adb)
		write_str(GADGETS "/adb_demo/UDC", "");
	write_str(A0 "/UDC", "");
	/* из конфигурации — накопитель (ссылку запомнить: вернуть) */
	dac_ms_link[0] = 0;
	DIR *d = opendir(A0 "/configs/c.1");
	struct dirent *e;
	while (d && (e = readdir(d)))
		if (!strncmp(e->d_name, "mass_storage", 12)) {
			char path[160];
			snprintf(dac_ms_link, sizeof(dac_ms_link), "%s", e->d_name);
			snprintf(path, sizeof(path), A0 "/configs/c.1/%s", e->d_name);
			unlink(path);
		}
	if (d)
		closedir(d);
	if (mkdir(UAC, 0755) && errno != EEXIST) {
		perror("usb: uac_sa");
		return 0;
	}
	write_str(UAC "/c_chmask", "0x03");
	write_str(UAC "/c_ssize", "2");
	write_str(UAC "/c_srate", "32000,44100,48000,88200,96000,176400,192000,352800,384000");
	write_str(UAC "/p_chmask", "0");   /* без записи: микрофон, которого никто не кормит, хост может отвергнуть */
	for (int i = 0; i < 3; i++) {   /* 239/2/1 — составное устройство со звуком */
		read_line(dac_class_attr[i], dac_class[i], sizeof(dac_class[i]));
		write_str(dac_class_attr[i], i == 0 ? "239" : i == 1 ? "2" : "1");
	}
	if (symlink(UAC, A0 "/configs/c.1/uac_sa.a") && errno != EEXIST)
		perror("usb: uac_sa link");
	write_str(A0 "/UDC", usb_udc);
	/* без обратной связи по частоте часы компьютера и плеера расходятся, и компьютер обрывает поток */
	write_str("/sys/class/android_usb/f_uac_sa/dynamic_feedback", "1");
	/* компьютер перечитывает устройство только после «переподключения» — поднять и опустить подтяжку линии */
	if (!access("/sys/class/usb_gadget/android0/soft_disconnect", F_OK)) {
		write_str("/sys/class/usb_gadget/android0/soft_disconnect", "1");
		usleep(300000);
		write_str("/sys/class/usb_gadget/android0/soft_disconnect", "0");
	}
	for (int i = 0; i < 500 && access("/dev/uac_sa", F_OK); i++)
		usleep(10000);
	dac_fd = open("/dev/uac_sa", O_RDWR);
	if (dac_fd < 0) {
		perror("usb: /dev/uac_sa");
		return 0;
	}
	audio_set_usb_in(dac_fd);
	usb_dac = 1;
	printf("usb: ЦАП включён\n");
	return 1;
}

static void usb_dac_end(void)
{
	audio_set_usb_in(-1);
	if (dac_fd >= 0)
		close(dac_fd);
	dac_fd = -1;
	write_str(A0 "/UDC", "");
	unlink(A0 "/configs/c.1/uac_sa.a");
	rmdir(UAC);
	for (int i = 0; i < 3; i++)
		if (dac_class[i][0])
			write_str(dac_class_attr[i], dac_class[i]);
	if (dac_ms_link[0]) {
		char link[160], target[160];
		snprintf(link, sizeof(link), A0 "/configs/c.1/%s", dac_ms_link);
		snprintf(target, sizeof(target), A0 "/functions/%.40s", dac_ms_link);
		if (symlink(target, link))
			perror("usb: mass_storage link");
	}
	write_str(usb_adb ? GADGETS "/adb_demo/UDC" : A0 "/UDC", usb_udc);
	usb_dac = 0;
	xmb_dac_end();
	printf("usb: ЦАП выключен\n");
	fflush(stdout);
}

/* ---------- пульты: на проводе 3.5 (earpods_adc) и USB (адаптер-гарнитура, наушники USB-C) ----------
 * Номера eventN не постоянны (порядок загрузки модулей, USB появляется и пропадает) — опознаём по имени: earpods_* —
 * провод; неизвестное с кнопками «пауза» или «громкость» — USB. Средняя кнопка — по числу нажатий, как у Apple и
 * Sonix: одно — пауза, два — следующий трек, три — предыдущий (окно REM_CLICK_MS); громкость — шаг, удержание —
 * повтор. Кнопки на корпусе (event0, 2, 3) — свои, здесь их нет */
#define NREM 3
#define REM_CLICK_MS 300
#define REM_REPEAT_DELAY_MS 400
#define REM_REPEAT_MS 150
#define REM_EXTRA_MS 1000   /* лишнее нажатие после переключения трека — не пауза: в серии легко ошибиться на одно */
static struct {
	int fd, usb, clicks, held;
	uint32_t due, rep, track;
	char node[24], name[48];
} rem[NREM] = { { .fd = -1 }, { .fd = -1 }, { .fd = -1 } };
static int rem_inputs = -1;   /* устройств в /dev/input при последнем поиске */

static int key_bit(int fd, int code)
{
	unsigned long b[KEY_MAX / (8 * sizeof(long)) + 1] = { 0 };
	return ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(b)), b) >= 0 && (b[code / (8 * sizeof(long))] >> (code % (8 * sizeof(long))) & 1);
}

/* опрос кнопки на проводе драйвер начинает только после «on» в этот файл (иное — выключает; при загрузке выключен,
 * стоковый плеер включает его сам). Проверяем раз в секунду: после сна или переподключения может сброситься */
static void earpods_enable(void)
{
	static const char *sw = "/sys/devices/platform/earpods_adc/earpods_adc/earpods_adc_sw";
	char v = 0;
	int fd = open(sw, O_RDONLY);
	if (fd < 0)
		return;
	if (read(fd, &v, 1) != 1)
		v = 0;
	close(fd);
	if (v == '1')
		return;
	fd = open(sw, O_WRONLY);
	if (fd >= 0) {
		if (write(fd, "on", 2) == 2)
			printf("пульт: опрос кнопки на проводе 3.5 включён\n");
		close(fd);
	}
}

static void remote_scan(void)
{
	earpods_enable();
	DIR *d = opendir("/dev/input");
	if (!d)
		return;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)))
		n += !strncmp(e->d_name, "event", 5);
	if (n == rem_inputs) {
		closedir(d);
		return;
	}
	rem_inputs = n;
	rewinddir(d);
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char node[24];
		snprintf(node, sizeof(node), "/dev/input/%.12s", e->d_name);
		int have = 0, slot = -1;
		for (int i = 0; i < NREM; i++) {
			have |= rem[i].fd >= 0 && !strcmp(rem[i].node, node);
			if (rem[i].fd < 0 && slot < 0)
				slot = i;
		}
		if (have || slot < 0)
			continue;
		int fd = open(node, O_RDONLY | O_NONBLOCK);
		if (fd < 0)
			continue;
		char name[48] = { 0 };
		if (ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name) < 0)
			name[0] = 0;
		int wired = !strncmp(name, "earpods_", 8);
		int builtin = !strcmp(name, "md-gpio-keys") || !strcmp(name, "goodix-ts") || !strcmp(name, "jz adc keyboard") ||
			      !strcmp(name, "sa-ring-keys");
		if (!wired && (builtin || !(key_bit(fd, KEY_PLAYPAUSE) || key_bit(fd, KEY_VOLUMEUP)))) {
			close(fd);
			continue;
		}
		rem[slot].fd = fd;
		rem[slot].usb = !wired;
		rem[slot].clicks = rem[slot].held = 0;
		snprintf(rem[slot].node, sizeof(rem[slot].node), "%s", node);
		snprintf(rem[slot].name, sizeof(rem[slot].name), "%s", name);
		printf("пульт: %s — %s (%s)\n", node, name, wired ? "провод 3.5" : "USB");
	}
	closedir(d);
}

/* события пульта i; 0 — устройство пропало (USB вынули) */
static int remote_read(int i, uint32_t now)
{
	struct input_event ev;
	ssize_t r;
	while ((r = read(rem[i].fd, &ev, sizeof(ev))) == sizeof(ev)) {
		if (ev.type != EV_KEY || ev.value == 2 || (int32_t)(now - rem_quiet) < 0)
			continue;
		bb("пульт: клавиша %d %s", ev.code, ev.value ? "нажата" : "отпущена");
		switch (ev.code) {
		case KEY_PLAYPAUSE:
		case KEY_MEDIA:   /* гарнитуры Android */
			if (ev.value == 1) {
				rem[i].clicks++;
				rem[i].due = now + REM_CLICK_MS;
			}
			break;
		case KEY_NEXTSONG:   /* драйвер посчитал нажатия сам */
		case KEY_PREVIOUSSONG:
			if (ev.value == 1) {
				rem[i].clicks = 0;
				rem[i].track = now;
				xmb_input(ev.code == KEY_NEXTSONG ? XMB_NEXT_TRACK : XMB_PREV_TRACK);
			}
			break;
		case KEY_VOLUMEUP:
		case KEY_VOLUMEDOWN:
			if (ev.value == 1) {
				xmb_input(ev.code == KEY_VOLUMEUP ? XMB_VOL_UP : XMB_VOL_DOWN);
				rem[i].held = ev.code;
				rem[i].rep = now + REM_REPEAT_DELAY_MS;
			} else if (rem[i].held == ev.code) {
				rem[i].held = 0;
			}
			break;
		}
	}
	if (r < 0 && errno == ENODEV) {
		printf("пульт: %s — %s отключён\n", rem[i].node, rem[i].name);
		close(rem[i].fd);
		rem[i].fd = -1;
		rem_inputs = -1;
		return 0;
	}
	return 1;
}

/* досчитанные нажатия и повтор удержанной громкости; 1 — пульт ещё ждёт (часто опрашивать) */
static int remote_tick(uint32_t now)
{
	int busy = 0;
	for (int i = 0; i < NREM; i++) {
		if (rem[i].fd < 0)
			continue;
		if (rem[i].clicks && (int32_t)(now - rem[i].due) >= 0) {
			int c = rem[i].clicks;
			rem[i].clicks = 0;
			if (c > 1)
				rem[i].track = now;
			if (c > 1 || !rem[i].track || rem[i].due - REM_CLICK_MS - rem[i].track >= REM_EXTRA_MS)
				xmb_input(c == 1 ? XMB_PLAY : c == 2 ? XMB_NEXT_TRACK : XMB_PREV_TRACK);
		}
		if (rem[i].held && (int32_t)(now - rem[i].rep) >= 0) {
			xmb_input(rem[i].held == KEY_VOLUMEUP ? XMB_VOL_UP : XMB_VOL_DOWN);
			rem[i].rep = now + REM_REPEAT_MS;
		}
		busy |= rem[i].clicks || rem[i].held;
	}
	return busy;
}

/* XMB_SCRIPT="down,down,enter,wait,…" — сценарий для замеров без рук, шаг XMB_STEP мс (300); в конце — выход */
static char *script_pos;
static int script_done;
static char script_tok[24];   /* последнее нажатие сценария и когда — для журнала медленных кадров */
static uint32_t script_tok_ms;

static void script_step(lv_timer_t *t)
{
	(void)t;
	if (!script_pos || !*script_pos) {
		script_done = 1;
		return;
	}
	char *end = strchr(script_pos, ',');
	if (end)
		*end = 0;
	char *tok = script_pos;
	script_pos = end ? end + 1 : script_pos + strlen(script_pos);
	if (strcmp(tok, "wait")) {
		snprintf(script_tok, sizeof(script_tok), "%s", tok);
		script_tok_ms = tick_ms();
	}
	uint32_t t0 = tick_ms();
	if (!strcmp(tok, "mem"))   /* настоящий сон, как от колеса (на кабеле — только с XMB_FORCE_MEM) */
		deep_sleep();
	else
		xmb_input_name(tok);
	if (tick_ms() - t0 > 5)
		printf("ввод %s: %u мс\n", tok, tick_ms() - t0);
}

static uint32_t tick_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int panel_on, slow_pans, panel_fails;
static uint32_t prof_copy, prof_pan, prof_rows, prof_areas, prof_px;   /* за 5 с: мс на копирование в страницу, ожидание pan, строк, областей и пикселей отрисовки */
static uint32_t frame_t0, prof_slow, prof_max;   /* начало кадра; кадров дольше 20 мс (не успели к развёртке), самый долгий */
static int full_copy = 2;
static uint32_t refr_t0, refr_ms, pan_ms;   /* последний кадр: отрисовка LVGL (с копированием и pan) и ожидание pan */

static void refr_cb(lv_event_t *e)
{
	if (lv_event_get_code(e) == LV_EVENT_REFR_START)
		refr_t0 = tick_ms();
	else
		refr_ms = tick_ms() - refr_t0;
}

/* в заднюю страницу копируются строки, изменённые в этом и в прошлом кадре: она отстаёт на кадр.
 * Пока панель выключена (FBIOBLANK), кадры не выводятся: pan ждал бы таймаута драйвера. */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
	static int cur_y1 = H, cur_y2 = -1, prev_y1 = 0, prev_y2 = H - 1;
	(void)px;
	prof_areas++;
	prof_px += lv_area_get_size(area);
	cur_y1 = LV_MIN(cur_y1, area->y1);
	cur_y2 = LV_MAX(cur_y2, area->y2);
	if (lv_display_flush_is_last(disp) && !panel_on) {
		cur_y1 = H;
		cur_y2 = -1;
	} else if (lv_display_flush_is_last(disp)) {
		if (full_copy) {
			full_copy--;
			cur_y1 = prev_y1 = 0;
			cur_y2 = prev_y2 = H - 1;
		}
		fb_page = vi.yres_virtual >= 2 * vi.yres ? !fb_page : 0;
		uint8_t *dst = fb_mem + (size_t)fb_page * vi.yres * fi.line_length;
		int y1 = LV_MAX(0, LV_MIN(cur_y1, prev_y1)), y2 = LV_MIN(H - 1, LV_MAX(cur_y2, prev_y2));
		uint32_t tc = tick_ms();
		for (int y = y1; y <= y2; y++)
			memcpy(dst + y * fi.line_length, frame + y * W, W * 2);
		prof_copy += tick_ms() - tc;
		prof_rows += y2 >= y1 ? y2 - y1 + 1 : 0;
		prev_y1 = cur_y1;
		prev_y2 = cur_y2;
		cur_y1 = H;
		cur_y2 = -1;
		vi.yoffset = fb_page * vi.yres;
		uint32_t t0 = tick_ms();
		if (frame_t0) {   /* 0 — кадр включения экрана (с паузой на запуск панели), не в счёт */
			prof_slow += t0 - frame_t0 > 20;
			prof_max = t0 - frame_t0 > prof_max ? t0 - frame_t0 : prof_max;
		}
		ioctl(fb_fd, FBIOPAN_DISPLAY, &vi);
		pan_ms = tick_ms() - t0;
		prof_pan += pan_ms;
		slow_pans = tick_ms() - t0 > 200 ? slow_pans + 1 : 0;
		if (!slow_pans)
			panel_fails = 0;   /* кадр ушёл вовремя — панель жива */
		frames++;
	}
	lv_display_flush_ready(disp);
}

/* max_brightness драйвера = 101, но на 101 подсветка гаснет; рабочий максимум — 100 */
static int bl_level = 100;

static void bl_write(int v)
{
	FILE *f = fopen(BACKLIGHT, "w");
	if (!f)
		return;
	fprintf(f, "%d\n", v);
	fclose(f);
}

/* после FBIOBLANK драйвер подсветки считает её включённой, но железо выключено:
 * оно включается только записью нового значения, поэтому сначала 0 */
static void backlight(int on)
{
	bl_write(0);
	if (on)
		bl_write(bl_level);
}

/* после глубокого сна контроллер экрана не запускается сам (pan ждёт кадр до таймаута) — параметры экрана
 * применяются заново с FB_ACTIVATE_FORCE, драйвер перепрограммирует контроллер (так делает и Sonix) */
/* как у стока: гашение — подсветка 0, затем FBIOBLANK POWERDOWN; включение — UNBLANK, кадр, подсветка */
static void screen_set(int on)
{
	if (on) {
		frame_t0 = 0;
		ioctl(fb_fd, FBIOBLANK, FB_BLANK_UNBLANK);
		if (fb_reprogram) {   /* исходный режим (с двумя страницами): после сна драйвер может сбросить виртуальную высоту */
			struct fb_var_screeninfo v = vi;
			v.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
			if (ioctl(fb_fd, FBIOPUT_VSCREENINFO, &v) < 0)
				perror("fb: FBIOPUT_VSCREENINFO");
			if (ioctl(fb_fd, FBIOPAN_DISPLAY, &vi) < 0)
				perror("fb: FBIOPAN_DISPLAY");
			struct fb_var_screeninfo now;
			if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &now) == 0)
				printf("fb after wake: %ux%u virtual %ux%u\n", now.xres, now.yres, now.xres_virtual, now.yres_virtual);
			fb_reprogram = 0;
		}
		usleep(100000);
		panel_on = 1;
		slow_pans = 0;
		full_copy = 2;
		lv_obj_invalidate(lv_screen_active());
		lv_refr_now(NULL);
		backlight(1);
		if (woke_ms) {
			printf("экран после пробуждения: %u мс\n", tick_ms() - woke_ms);
			woke_ms = 0;
		}
	} else {
		bl_write(0);
		panel_on = 0;
		ioctl(fb_fd, FBIOBLANK, FB_BLANK_POWERDOWN);
	}
}

/* полный перезапуск панели: при старте и если вывод кадров уходит в таймаут */
static void panel_restart(void)
{
	fb_reprogram = 1;
	screen_set(0);
	screen_set(1);
}

void platform_brightness(int percent)
{
	bl_level = percent > 100 ? 100 : percent;
	if (xmb_screen_on())
		bl_write(bl_level);   /* экран и так включён: без промежуточного 0 (лишняя запись и мигание) */
}

/* заряд, зарядка, напряжение — фоновым потоком раз в 10 с: датчик отвечает по I2C, и после сна чтение sysfs
 * бывает тянется до секунды — главный цикл на нём вставал бы. Интерфейс берёт готовые значения */
static volatile int bat_cap = -1, bat_chg, bat_mv = -1;

static int read_int(const char *path)
{
	char buf[32];
	int v = -1;
	FILE *f = fopen(path, "r");
	if (f && fgets(buf, sizeof(buf), f))
		v = atoi(buf);
	if (f)
		fclose(f);
	return v;
}

/* заряд по напряжению покоя Li-ion — когда датчику нельзя верить */
static int mv_to_cap(int mv)
{
	static const int pts[][2] = { { 3400, 0 }, { 3600, 8 }, { 3700, 25 }, { 3750, 40 }, { 3800, 55 }, { 3900, 70 },
				      { 4000, 82 }, { 4100, 92 }, { 4180, 100 } };
	int n = sizeof(pts) / sizeof(pts[0]);
	if (mv <= pts[0][0])
		return 0;
	for (int i = 1; i < n; i++)
		if (mv < pts[i][0])
			return pts[i - 1][1] + (pts[i][1] - pts[i - 1][1]) * (mv - pts[i - 1][0]) / (pts[i][0] - pts[i - 1][0]);
	return 100;
}

static void *bat_thread(void *arg)
{
	(void)arg;
	static const char *names[] = { "battery", "cw2015", "axp2101-battery", "bat" };
	char path[96], buf[32];
	for (;;) {
		for (int i = 0; i < 4; i++) {
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/capacity", names[i]);
			int cap = read_int(path);
			if (cap < 0)
				continue;
			bat_cap = cap;
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/status", names[i]);
			FILE *f = fopen(path, "r");
			if (f) {
				bat_chg = fgets(buf, sizeof(buf), f) && (!strncmp(buf, "Charging", 8) || !strncmp(buf, "Full", 4));
				fclose(f);
			}
			snprintf(path, sizeof(path), "/sys/class/power_supply/%s/voltage_now", names[i]);
			int uv = read_int(path);
			bat_mv = uv > 0 ? uv / 1000 : -1;
			/* CW2015 бывает залипает на 0 % при живой батарее (Sonix сбрасывает его за это в своём драйвере) */
			static int stuck_logged;
			if (cap == 0 && bat_mv > 3600) {
				bat_cap = mv_to_cap(bat_mv);
				if (!stuck_logged) {
					stuck_logged = 1;
					printf("батарея: датчик показывает 0 %% при %d мВ — заряд по напряжению\n", bat_mv);
				}
			}
			break;
		}
		sleep(10);
	}
	return NULL;
}

int platform_battery(int *charging)
{
	*charging = bat_chg;
	return bat_cap;
}

int platform_battery_mv(void)
{
	return bat_mv;
}

int main(int argc, char **argv)
{
	/* журнал открывает сам XMB (XMB_LOG): перенаправление в run_xmb.sh держит файл на карте открытым в shell,
	 * и карту не отмонтировать для USB-накопителя; рабочая папка — не карта и не configfs */
	const char *log = getenv("XMB_LOG");
	int lfd = log ? open(log, O_WRONLY | O_CREAT | O_APPEND, 0644) : -1;
	if (lfd >= 0) {
		dup2(lfd, 1);
		dup2(lfd, 2);
		close(lfd);
	}
	setvbuf(stdout, NULL, _IOLBF, 0);
	if (chdir("/") < 0)
		perror("chdir");
	fb_fd = open("/dev/fb0", O_RDWR);
	if (fb_fd < 0) { perror("fb0"); return 1; }
	ioctl(fb_fd, FBIOGET_VSCREENINFO, &vi);
	ioctl(fb_fd, FBIOGET_FSCREENINFO, &fi);
	fb_mem = mmap(NULL, fi.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
	if (fb_mem == MAP_FAILED) { perror("mmap"); return 1; }

	pthread_t bt_;
	pthread_create(&bt_, NULL, bat_thread, NULL);
	pthread_detach(bt_);
	lv_init();
	lv_tick_set_cb(tick_ms);
	lv_display_t *disp = lv_display_create(W, H);
	lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
	lv_display_set_buffers(disp, frame, NULL, sizeof(frame), LV_DISPLAY_RENDER_MODE_DIRECT);
	lv_display_set_flush_cb(disp, flush_cb);
	lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_START, NULL);
	lv_display_add_event_cb(disp, refr_cb, LV_EVENT_REFR_READY, NULL);
	lv_evdev_create(LV_INDEV_TYPE_POINTER, "/dev/input/event1");

	xmb_init(argc > 1 ? argv[1] : "/tmp/xmb/fonts");
	const char *warm = getenv("XMB_WARM");
	if (warm && *warm) {   /* перезапуск из run_xmb.sh: панель уже работает — не гасить, кадр прошлого процесса */
		panel_on = 1;            /* остаётся на экране, пока не нарисуется новый */
		full_copy = 2;
	} else {
		panel_restart();
	}
	{
		char lun[64];
		if (read_line(LUN, lun, sizeof(lun)))
			write_str(LUN, "");   /* система при загрузке оставляет карту в картридере, пока она смонтирована у нас */
		DIR *d = opendir("/sys/class/udc");   /* контроллер USB — единственный */
		struct dirent *e;
		while (d && (e = readdir(d)))
			if (e->d_name[0] != '.') {
				snprintf(usb_udc, sizeof(usb_udc), "%s", e->d_name);
				break;
			}
		if (d)
			closedir(d);
	}
	if (getenv("XMB_SCRIPT")) {
		audio_set_volume(0);   /* замеры — без звука; настройка громкости не меняется */
		script_pos = strdup(getenv("XMB_SCRIPT"));
		lv_timer_create(script_step, getenv("XMB_STEP") ? atoi(getenv("XMB_STEP")) : 300, NULL);
	}

	struct pollfd pfd[3 + NREM];
	const char *keys[3] = { "/dev/input/event0", "/dev/input/event2", "/dev/input/event3" };
	for (int i = 0; i < 3; i++) {
		pfd[i].fd = open(keys[i], O_RDONLY | O_NONBLOCK);
		pfd[i].events = POLLIN;
	}
	remote_scan();
	key_fd = pfd[0].fd;
	signal(SIGUSR1, shot_sig);

	uint32_t sel_down = 0;
	int sel_long = 0;
	/* у кнопки экрана долгого жеста нет */
	btn_t b_next = { XMB_BTN_NEXT, 1 }, b_play = { XMB_BTN_PLAY, 1 }, b_screen = { XMB_BTN_SCREEN, 0 };
	int screen = 1;
	uint32_t t_fps = tick_ms(), off_since = tick_ms(), wake_until = 0;   /* после сна: разбудившее нажатие — до отпускания */
	uint32_t auto_cd = 0;   /* начало отсчёта перед автоматическим сном, 0 — нет */
	uint32_t sleep_fail = 0;   /* когда ядро последний раз отказало в сне */
	uint32_t usb_ms = 0;
	int host = usb_host(), exit_code = 0;   /* кабель, подключённый до запуска, — без диалога */
	int sd_heal = 0;   /* попыток вернуть карту, отнятую стоком */
	xmb_usb_host(host, 0);

	uint32_t prof_tick = 0, prof_poll = 0;
	for (;;) {
		uint32_t t_h = frame_t0 = tick_ms();
		uint32_t wait = lv_timer_handler();
		uint32_t t_p = tick_ms();
		prof_tick += t_p - t_h;
		if (script_pos && t_p - t_h > 30)
			printf("медленно %u мс: %s +%u мс (рисование %u, из него pan %u)\n", t_p - t_h, script_tok, t_h - script_tok_ms,
			       refr_ms, pan_ms);
		refr_ms = pan_ms = 0;
		/* экран погашен — кадров нет, просыпаться 4 раза в секунду (кнопки будят poll сразу); пока кнопка нажата или
		 * ждёт второго нажатия — чаще: жест решается по времени */
		int busy = b_next.held || b_next.pending || b_play.held || b_play.pending || b_screen.held || b_screen.pending ||
			   sel_down || auto_cd || remote_tick(tick_ms());
		int cap = screen ? 16 : busy ? 20 : 250;
		for (int i = 0; i < NREM; i++) {
			pfd[3 + i].fd = rem[i].fd;
			pfd[3 + i].events = POLLIN;
		}
		poll(pfd, 3 + NREM, screen && wait < 16 ? (int)wait : cap);
		prof_poll += tick_ms() - t_p;

		uint32_t now = tick_ms();
		for (int i = 0; i < NREM; i++)
			if (rem[i].fd >= 0 && !usb_storage && remote_read(i, now) && auto_cd && rem[i].clicks) {
				auto_cd = 0;   /* отсчёт перед автосном: нажатие на пульте отменяет его */
				xmb_sleep_countdown(-1, NULL);
				rem[i].clicks = 0;
			}
		for (int i = 0; i < 3; i++) {
			struct input_event ev;
			while (pfd[i].fd >= 0 && read(pfd[i].fd, &ev, sizeof(ev)) == sizeof(ev)) {
				if (ev.type != EV_KEY)
					continue;
				if (usb_storage && ev.code != KEY_POWER && ev.code != KEY_LEFT && ev.code != KEY_RIGHT)
					continue;   /* в режиме накопителя работает только колесо */
				if (wake_until) {   /* после сна: разбудившее нажатие только включает экран — глотаем до отпускания */
					if (ev.value == 0 || now > wake_until)
						wake_until = 0;
					if (ev.value != 1 || now <= wake_until)
						continue;
				}
				if (auto_cd) {   /* отсчёт перед автосном: любая кнопка только отменяет его */
					auto_cd = 0;
					xmb_sleep_countdown(-1, NULL);
					sel_down = 0;
					continue;
				}
				switch (ev.code) {
				case KEY_LEFT:  if (ev.value == 1) xmb_input(XMB_WHEEL_DOWN); break;
				case KEY_RIGHT: if (ev.value == 1) xmb_input(XMB_WHEEL_UP); break;
				case KEY_PREVIOUSSONG:   /* кнопка «след. трек» на корпусе */
					if (ev.value == 1)
						bb("кнопка: след. трек");
					btn_event(&b_next, ev.value, now);
					break;
				case KEY_PLAYPAUSE:
					if (ev.value == 1)
						bb("кнопка: играть");
					btn_event(&b_play, ev.value, now);
					break;
				case KEY_POWER:   /* клик колесом: короткий — выбор (при отпускании), долгий — действия с пунктом */
					if (ev.value == 1) {
						sel_down = now;
						sel_long = 0;
					} else if (ev.value == 0 && sel_down && usb_storage) {
						sel_down = 0;   /* в режиме накопителя клик только будит экран; карта — прокруткой (или удержанием) */
						if (!xmb_screen_on())
							xmb_input(XMB_SELECT);
					} else if (ev.value == 0 && sel_down) {
						if (now - sel_down >= SLEEP_HOLD_MS)
							xmb_sleep_countdown(-1, NULL);   /* отпустили до конца отсчёта — не спим */
						else if (!sel_long)
							xmb_input(XMB_SELECT);
						sel_down = 0;
					}
					break;
				case KEY_NEXTSONG:   /* кнопка «пред. трек» на корпусе — экран */
					btn_event(&b_screen, ev.value, now);
					break;
				}
			}
		}
		if ((script_done && !usb_storage) || xmb_quit_requested())
			break;
		if (xmb_power_off_requested()) {
			exit_code = XMB_POWER_OFF;
			break;
		}
		if (xmb_restart_requested()) {
			exit_code = XMB_RESTART;
			break;
		}
		/* USB: раз в секунду — компьютер подключён или отключён; в режиме накопителя — его конец */
		if (now - usb_ms >= (usb_storage ? 500u : 1000u)) {
			usb_ms = now;
			remote_scan();
			/* карта пропала не по нашей воле: сток при загрузке с кабелем отдаёт её под накопитель, уже когда XMB
			 * дождался карты и запустился. Вернуть себе и начать заново — библиотеке нужны файлы */
			if (!usb_storage && sd_heal < 3 && !sd_mounted()) {
				sd_heal++;
				write_str(LUN, "");
				if (mount(SD_DEV, SD_MNT, "exfat", MS_RELATIME, SD_OPTS) == 0) {
					printf("карта: была отмонтирована — смонтирована заново\n");
					exit_code = XMB_RESTART;
					break;
				}
				perror("карта: mount");
			}
			char ac[8];
			xmb_set_cable(usb_power() || (read_line("/sys/class/power_supply/ac/online", ac, sizeof(ac)) && !strcmp(ac, "1")));
			int h = usb_host();
			if (h != host && !usb_storage && !usb_dac)
				xmb_usb_host(h, 1);
			host = h;
			char lun[64];
			/* конец: удержание колеса, кабель вынут (по питанию: перечисление после смены гаджета на миг сбрасывает «configured»),
			 * компьютер извлёк диск (ядро очищает file) */
			if (usb_storage && (usb_storage == 2 || !usb_power() || !read_line(LUN, lun, sizeof(lun)) || xmb_usb_end_requested())) {
				usb_storage_end();
				exit_code = XMB_RESTART;
				break;
			}
			/* ЦАП: удержание колеса, кабель вынут или выход из интерфейса */
			if (usb_dac && !usb_power())
				usb_dac_end();
			xmb_set_usb_link(usb_dac ? XMB_USB_DAC : !h || usb_storage ? XMB_USB_NONE
					 : read_line(GADGETS "/adb_demo/UDC", lun, sizeof(lun)) ? XMB_USB_ADB : XMB_USB_PC);
		}
		if (usb_dac && xmb_usb_end_requested())
			usb_dac_end();
		if (xmb_usb_dac_requested() && host && !usb_storage && !usb_dac && !usb_dac_begin())
			usb_dac_end();   /* не вышло — вернуть всё как было */
		if (xmb_usb_storage_requested() && host && !usb_storage && !usb_storage_begin()) {
			exit_code = XMB_RESTART;   /* всё уже закрыто — проще начать заново */
			break;
		}

		btn_tick(&b_next, now);
		btn_tick(&b_play, now);
		btn_tick(&b_screen, now);
		if (sel_down && !sel_long && !usb_storage && now - sel_down >= HOLD_PREV_MS) {
			sel_long = 1;
			xmb_input(XMB_MENU);
		}
		/* удержание колеса дольше — отсчёт до сна, как в стоке: отпустить — отмена */
		if (sel_down && usb_storage && now - sel_down >= USB_HOLD_MS)
			usb_storage = 2;   /* удержание колеса: карта — обратно плееру (клик слишком легко задеть) */
		if (sel_down && !usb_storage && now - sel_down >= SLEEP_HOLD_MS) {
			int left = SLEEP_HOLD_MS + SLEEP_COUNT_MS - (int)(now - sel_down);
			if (!xmb_screen_on())
				xmb_wake();
			if (left > 0) {
				xmb_sleep_countdown((left + SLEEP_COUNT_MS / 3 - 1) / (SLEEP_COUNT_MS / 3), "отпустите колесо — отмена");
			} else {
				sel_down = 0;
				if (!deep_sleep())
					sleep_fail = tick_ms();
				screen = 0;
				wake_until = tick_ms() + 1500;
				sel_down = 0;
				off_since = tick_ms();
			}
		}

		if (xmb_screen_on() != screen) {
			screen = xmb_screen_on();
			screen_set(screen);
		}
		/* автосон: экран погашен, музыка давно не играет — экран загорается с отсчётом, любая кнопка или касание
		 * отменяет, иначе сон. Пока на кабеле компьютер (adb, накопитель) — не спим: сон рвёт USB, а adbd после
		 * него перестаёт открывать оболочку; отсчёт — с отключения кабеля */
		if ((screen || host) && !auto_cd)
			off_since = now;
		uint32_t after = xmb_sleep_after_ms();
		/* сон не удался — следующая попытка не раньше чем через 5 мин (иначе экран загорался бы каждые полминуты) */
		if (!screen && after && now - off_since >= after && !auto_cd && !mem_blocked &&
		    (!sleep_fail || now - sleep_fail > 300000)) {
			xmb_wake();
			auto_cd = tick_ms();
		}
		if (auto_cd && (xmb_last_input() > auto_cd || host)) {   /* касание или подключили к компьютеру */
			auto_cd = 0;
			xmb_sleep_countdown(-1, NULL);
			off_since = now;
		} else if (auto_cd) {
			int left = AUTO_COUNT_MS - (int)(now - auto_cd);
			if (left > 0) {
				xmb_sleep_countdown((left + 999) / 1000, "нажмите любую кнопку — отмена");
			} else {
				auto_cd = 0;
				if (!deep_sleep())
					sleep_fail = tick_ms();
				screen = 0;
				wake_until = tick_ms() + 1500;
				sel_down = 0;
				off_since = tick_ms();
			}
		}
		/* контроллер, сломанный сном с невыключенной панелью, перезапуск не лечит — только перезагрузка плеера */
		if (panel_on && slow_pans >= 3 && panel_fails < 2) {
			printf("pan timeouts, restarting panel\n");
			panel_restart();
			if (++panel_fails == 2)
				xmb_screen_broken();
		}

		if (shot_req) {
			shot_req = 0;
			FILE *f = fopen("/tmp/xmb_shot.raw", "wb");
			if (f) {
				fwrite(frame, sizeof(frame), 1, f);
				fclose(f);
			}
		}
		if (now - t_fps >= 5000) {
			/* на кадр: lvgl (рисование, с копированием и pan внутри), копирование, ожидание pan, сон в poll */
			int f = frames ? frames : 1;
			printf("fps %.1f  lvgl %.1f copy %.1f pan %.1f poll %.1f ms/кадр, строк %u, медленных %u, макс %u мс, областей %u, пикс %u\n",
			       frames * 1000.0 / (now - t_fps), (double)prof_tick / f, (double)prof_copy / f, (double)prof_pan / f,
			       (double)prof_poll / f, prof_rows / f, prof_slow, prof_max, prof_areas / f, prof_px / f);
			fflush(stdout);
			prof_tick = prof_poll = prof_copy = prof_pan = prof_rows = prof_slow = prof_max = prof_areas = prof_px = 0;
			frames = 0;
			t_fps = now;
		}
	}
	if (exit_code == XMB_RESTART)
		xmb_restart_prepare();   /* новый процесс раскроет меню там же */
	fsave_flush();   /* заказанные записи настроек и очереди — до выхода */
	screen_set(1);
	return exit_code;
}
