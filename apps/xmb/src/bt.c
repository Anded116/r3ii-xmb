/* bt.c — Bluetooth: стоковые bluetoothd, bt-agent (сопряжение без PIN) и bluealsa (A2DP-источник) уже запущены
 * прошивкой; здесь — их команды из фонового потока. Раз в 2 с поток смотрит PCM BlueALSA: появился приёмник —
 * звук уходит в него, пропал — в разъём. Список устройств перечитывается после операций и по просьбе интерфейса. */
#define _GNU_SOURCE
#include "bt.h"
#include "subproc.h"

#include "audio.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

enum { OP_NONE, OP_POWER, OP_SCAN, OP_CONNECT, OP_DISCONNECT, OP_CODEC };

static struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int op, op_on;
	char op_arg[64];
	int power, refresh;
	bt_dev_t dev[BT_MAX];
	int ndev;
	unsigned gen, msg_seq;
	char status[48], msg[96];
	char pcm[160];   /* выход звука сейчас */
} b = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

#if XMB_DEVICE
/* строки вывода команды — в cb; код возврата */
static int run(const char *cmd, void (*cb)(const char *line, void *u), void *u)
{
	FILE *f = sp_popen(cmd, "r");
	if (!f)
		return -1;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (cb)
			cb(line, u);
	}
	return sp_pclose(f);
}

static void set_status(const char *s)
{
	pthread_mutex_lock(&b.mu);
	snprintf(b.status, sizeof(b.status), "%s", s);
	b.gen++;
	pthread_mutex_unlock(&b.mu);
}

static void set_msg(const char *s)
{
	pthread_mutex_lock(&b.mu);
	snprintf(b.msg, sizeof(b.msg), "%s", s);
	b.msg_seq++;
	b.gen++;
	pthread_mutex_unlock(&b.mu);
}

typedef struct {
	bt_dev_t v[BT_MAX];
	int n;
} devlist_t;

/* «Device 3C:B0:ED:5E:98:B7 Имя» */
static void dev_line(const char *line, void *u)
{
	devlist_t *l = u;
	if (strncmp(line, "Device ", 7) || strlen(line) < 24 || l->n >= BT_MAX)
		return;
	for (int i = 0; i < l->n; i++)
		if (!strncmp(l->v[i].mac, line + 7, 17))
			return;
	bt_dev_t *d = &l->v[l->n++];
	memset(d, 0, sizeof(*d));
	memcpy(d->mac, line + 7, 17);
	snprintf(d->name, sizeof(d->name), "%s", line[24] ? line + 25 : d->mac);
}

static void info_line(const char *line, void *u)
{
	bt_dev_t *d = u;
	const char *p = line;
	while (*p == ' ' || *p == '\t')
		p++;
	if (!strcmp(p, "Paired: yes"))
		d->paired = 1;
	else if (!strcmp(p, "Connected: yes"))
		d->connected = 1;
}

static int dev_rank(const bt_dev_t *d)
{
	return d->connected ? 0 : d->paired ? 1 : 2;
}

static int dev_cmp(const void *x, const void *y)
{
	const bt_dev_t *p = x, *q = y;
	int r = dev_rank(p) - dev_rank(q);
	return r ? r : strcasecmp(p->name, q->name);
}

static void refresh_devices(void)
{
	static devlist_t t;
	t.n = 0;
	run("bluetoothctl devices 2>/dev/null", dev_line, &t);
	for (int i = 0; i < t.n; i++) {
		char cmd[64];
		snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", t.v[i].mac);
		run(cmd, info_line, &t.v[i]);
	}
	qsort(t.v, t.n, sizeof(bt_dev_t), dev_cmp);
	pthread_mutex_lock(&b.mu);
	memcpy(b.dev, t.v, sizeof(t.v));
	b.ndev = t.n;
	b.gen++;
	pthread_mutex_unlock(&b.mu);
}

/* PCM приёмника A2DP: /org/bluealsa/hci0/dev_XX_XX_XX_XX_XX_XX/a2dpsrc/sink */
static void pcm_line(const char *line, void *u)
{
	if (strstr(line, "/a2dpsrc/sink") && !*(char *)u)
		snprintf(u, 160, "%s", line);
}

static void fmt_line(const char *line, void *u)
{
	int *v = u;   /* частота, бит */
	if (!strncmp(line, "Format:", 7))
		v[1] = strstr(line, "S24") ? 24 : strstr(line, "S32") ? 32 : 16;
	else if (!strncmp(line, "Sampling:", 9) || !strncmp(line, "Rate:", 5))
		v[0] = atoi(strchr(line, ':') + 1);
}

static void watch_pcm(void)
{
	char pcm[160] = "";
	if (b.power)
		run("bluealsa-cli -q list-pcms 2>/dev/null", pcm_line, pcm);
	if (!strcmp(pcm, b.pcm))
		return;
	int v[2] = { 44100, 16 };
	if (pcm[0]) {
		char cmd[200];
		snprintf(cmd, sizeof(cmd), "bluealsa-cli -q info %s 2>/dev/null", pcm);
		run(cmd, fmt_line, v);
	}
	printf("bt: выход — %s (%d Гц, %d бит)\n", pcm[0] ? pcm : "разъём", v[0], v[1]);
	audio_set_bt(pcm[0] ? pcm : NULL, v[0], v[1]);
	snprintf(b.pcm, sizeof(b.pcm), "%s", pcm);
	b.refresh = 1;   /* подключение видно в списке */
}

static void do_op(int op, int on, const char *arg)
{
	char cmd[160];
	switch (op) {
	case OP_POWER:
		snprintf(cmd, sizeof(cmd), "bluetoothctl power %s > /dev/null 2>&1", on ? "on" : "off");
		run(cmd, NULL, NULL);
		b.power = on;
		break;
	case OP_SCAN:
		set_status("поиск устройств…");
		run("bluetoothctl --timeout 10 scan on > /dev/null 2>&1", NULL, NULL);
		set_status("");
		break;
	case OP_CONNECT: {
		bt_dev_t d = { 0 };
		snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", arg);
		run(cmd, info_line, &d);
		set_status("подключение…");
		if (!d.paired) {
			snprintf(cmd, sizeof(cmd), "bluetoothctl --timeout 15 pair %s > /dev/null 2>&1", arg);
			run(cmd, NULL, NULL);
		}
		snprintf(cmd, sizeof(cmd), "bluetoothctl trust %s > /dev/null 2>&1", arg);
		run(cmd, NULL, NULL);
		snprintf(cmd, sizeof(cmd), "bluetoothctl --timeout 15 connect %s > /dev/null 2>&1", arg);
		run(cmd, NULL, NULL);
		memset(&d, 0, sizeof(d));
		snprintf(cmd, sizeof(cmd), "bluetoothctl info %s 2>/dev/null", arg);
		run(cmd, info_line, &d);
		set_status("");
		set_msg(d.connected ? "Подключено" : "Не удалось подключить");
		break;
	}
	case OP_DISCONNECT:
		snprintf(cmd, sizeof(cmd), "bluetoothctl disconnect %s > /dev/null 2>&1", arg);
		run(cmd, NULL, NULL);
		set_msg("Отключено");
		break;
	case OP_CODEC:
		if (b.pcm[0] && arg[0]) {
			snprintf(cmd, sizeof(cmd), "bluealsa-cli -q codec %s %s 2>/dev/null", b.pcm, arg);
			run(cmd, NULL, NULL);
			b.pcm[0] = 0;   /* формат мог смениться: выход перечитается */
		}
		break;
	}
	b.refresh = 1;
}

static void *bt_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { 0 };
	pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
	for (;;) {
		pthread_mutex_lock(&b.mu);
		if (!b.op) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 2;
			pthread_cond_timedwait(&b.cv, &b.mu, &ts);
		}
		int op = b.op, on = b.op_on;
		char a[64];
		snprintf(a, sizeof(a), "%s", b.op_arg);
		b.op = OP_NONE;
		pthread_mutex_unlock(&b.mu);
		if (op)
			do_op(op, on, a);
		if (b.power || b.pcm[0])
			watch_pcm();
		if (b.refresh) {
			b.refresh = 0;
			refresh_devices();
		}
	}
	return NULL;
}

static void post(int op, int on, const char *arg)
{
	pthread_mutex_lock(&b.mu);
	b.op = op;
	b.op_on = on;
	snprintf(b.op_arg, sizeof(b.op_arg), "%s", arg ? arg : "");
	pthread_cond_signal(&b.cv);
	pthread_mutex_unlock(&b.mu);
}

void bt_init(int on)
{
	pthread_t t;
	b.op = OP_POWER;
	b.op_on = on;
	pthread_create(&t, NULL, bt_thread, NULL);
	pthread_detach(t);
}
#else
static void post(int op, int on, const char *arg)
{
	(void)op;
	(void)on;
	(void)arg;
}

void bt_init(int on)
{
	b.power = on;
}
#endif

void bt_power(int on)
{
	post(OP_POWER, on, NULL);
}

void bt_scan(void)
{
	post(OP_SCAN, 0, NULL);
}

void bt_connect(const char *mac)
{
	post(OP_CONNECT, 0, mac);
}

void bt_disconnect(const char *mac)
{
	post(OP_DISCONNECT, 0, mac);
}

void bt_set_codec(const char *name)
{
	post(OP_CODEC, 0, name);
}

int bt_devices(bt_dev_t *out, int max)
{
	pthread_mutex_lock(&b.mu);
	int n = b.ndev < max ? b.ndev : max;
	memcpy(out, b.dev, sizeof(bt_dev_t) * n);
	pthread_mutex_unlock(&b.mu);
	return n;
}

unsigned bt_gen(void)
{
	return b.gen;
}

const char *bt_status(void)
{
	return b.status;
}

const char *bt_message(unsigned *seq)
{
	*seq = b.msg_seq;
	return b.msg;
}
