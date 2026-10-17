/* wifi.c — фоновый поток над wpa_supplicant (конфиг стока /data/wpa_supplicant.conf, update_config=1: новые сети
 * сохраняются туда же), wpa_cli и udhcpc. Раз в 3 с — состояние; подключилось — адрес по DHCP и время по NTP. */
#define _GNU_SOURCE
#include "wifi.h"
#include "spawn.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define CONF "/data/wpa_supplicant.conf"
#define CLI "wpa_cli -i wlan0 "

enum { OP_NONE, OP_POWER, OP_SCAN, OP_CONNECT, OP_FORGET };

static struct {
	pthread_mutex_t mu;
	pthread_cond_t cv;
	int op, op_on;
	char op_ssid[33], op_psk[64];
	int power, online, refresh;
	char ssid[33];   /* к какой сети подключены */
	wifi_net_t net[WIFI_MAX];
	int nnet;
	unsigned gen, msg_seq;
	char status[48], msg[96];
} w = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

#if XMB_DEVICE
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
	pthread_mutex_lock(&w.mu);
	snprintf(w.status, sizeof(w.status), "%s", s);
	w.gen++;
	pthread_mutex_unlock(&w.mu);
}

static void set_msg(const char *s)
{
	pthread_mutex_lock(&w.mu);
	snprintf(w.msg, sizeof(w.msg), "%s", s);
	w.msg_seq++;
	w.gen++;
	pthread_mutex_unlock(&w.mu);
}

/* строка для shell в одинарных кавычках; dq — ещё и в двойных внутри (так wpa_cli ждёт имя сети и пароль) */
static void quoted_as(char *out, int cap, const char *s, int dq)
{
	int k = 0;
	out[k++] = '\'';
	if (dq)
		out[k++] = '"';
	for (; *s && k < cap - 7; s++) {
		if (*s == '\'') {
			memcpy(out + k, "'\\''", 4);
			k += 4;
		} else {
			out[k++] = *s;
		}
	}
	if (dq)
		out[k++] = '"';
	out[k++] = '\'';
	out[k] = 0;
}

static void quoted(char *out, int cap, const char *s)
{
	quoted_as(out, cap, s, 1);
}

typedef struct {
	char state[24], ssid[33], ip[20];
} st_t;

static void status_line(const char *line, void *u)
{
	st_t *s = u;
	if (!strncmp(line, "wpa_state=", 10))
		snprintf(s->state, sizeof(s->state), "%s", line + 10);
	else if (!strncmp(line, "ssid=", 5))
		snprintf(s->ssid, sizeof(s->ssid), "%s", line + 5);
	else if (!strncmp(line, "ip_address=", 11))
		snprintf(s->ip, sizeof(s->ip), "%s", line + 11);
}

typedef struct {
	wifi_net_t v[WIFI_MAX];
	int n;
} netlist_t;

/* «bssid freq signal flags ssid» — по табуляции; одинаковые имена (несколько точек) — одна строка */
static void scan_line(const char *line, void *u)
{
	netlist_t *l = u;
	char f[5][80] = { { 0 } };
	int k = 0;
	for (const char *p = line; k < 5; k++) {
		const char *e = strchr(p, '\t');
		int n = e && k < 4 ? (int)(e - p) : (int)strlen(p);
		snprintf(f[k], sizeof(f[k]), "%.*s", n, p);
		if (!e || k == 4)
			break;
		p = e + 1;
	}
	if (k < 4 || !f[4][0] || strchr(f[0], '/'))
		return;
	int sig = atoi(f[2]);
	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->v[i].ssid, f[4])) {
			if (sig > l->v[i].signal)
				l->v[i].signal = sig;
			return;
		}
	if (l->n >= WIFI_MAX)
		return;
	wifi_net_t *n = &l->v[l->n++];
	memset(n, 0, sizeof(*n));
	snprintf(n->ssid, sizeof(n->ssid), "%s", f[4]);
	n->signal = sig;
	n->secured = strstr(f[3], "WPA") || strstr(f[3], "WEP");
}

/* «id ssid bssid flags» — сохранённые */
static void saved_line(const char *line, void *u)
{
	netlist_t *l = u;
	const char *t = strchr(line, '\t');
	if (!t || !strncmp(line, "network id", 10))
		return;
	char ssid[33];
	const char *e = strchr(t + 1, '\t');
	snprintf(ssid, sizeof(ssid), "%.*s", e ? (int)(e - t - 1) : (int)strlen(t + 1), t + 1);
	for (int i = 0; i < l->n; i++)
		if (!strcmp(l->v[i].ssid, ssid))
			l->v[i].saved = 1;
}

static int net_rank(const wifi_net_t *n)
{
	return n->connected ? 0 : n->saved ? 1 : 2;
}

static int net_cmp(const void *a, const void *b)
{
	const wifi_net_t *x = a, *y = b;
	int r = net_rank(x) - net_rank(y);
	return r ? r : y->signal - x->signal;
}

static void refresh_list(void)
{
	static netlist_t l;
	l.n = 0;
	run(CLI "scan_results 2>/dev/null", scan_line, &l);
	run(CLI "list_networks 2>/dev/null", saved_line, &l);
	for (int i = 0; i < l.n; i++)
		l.v[i].connected = w.online && !strcmp(l.v[i].ssid, w.ssid);
	qsort(l.v, l.n, sizeof(wifi_net_t), net_cmp);
	pthread_mutex_lock(&w.mu);
	memcpy(w.net, l.v, sizeof(l.v));
	w.nnet = l.n;
	w.gen++;
	pthread_mutex_unlock(&w.mu);
}

/* подключились — адрес по DHCP (стоковый udhcpc сам пишет resolv.conf), потом время по NTP и в RTC */
static void bring_up(const char *ssid)
{
	set_status("получение адреса…");
	int ok = run("udhcpc -i wlan0 -q -n -t 6 > /dev/null 2>&1", NULL, NULL) == 0;
	set_status("");
	pthread_mutex_lock(&w.mu);
	w.online = ok;
	snprintf(w.ssid, sizeof(w.ssid), "%s", ssid);
	w.gen++;
	pthread_mutex_unlock(&w.mu);
	printf("wifi: %s — %s\n", ssid, ok ? "адрес получен" : "нет адреса по DHCP");
	fflush(stdout);
	if (!ok)
		return;
	char m[96];
	snprintf(m, sizeof(m), "Wi-Fi: %s", ssid);
	set_msg(m);
	if (run("ntpdate -p 2 -t 2 0.pool.ntp.org time.google.com > /dev/null 2>&1", NULL, NULL) == 0)
		run("hwclock -w > /dev/null 2>&1", NULL, NULL);
}

static void poll_state(void)
{
	st_t s = { "", "", "" };
	run(CLI "status 2>/dev/null", status_line, &s);
	int up = !strcmp(s.state, "COMPLETED");
	if (up && !w.online)
		bring_up(s.ssid);
	else if (!up && w.online) {
		pthread_mutex_lock(&w.mu);
		w.online = 0;
		w.gen++;
		pthread_mutex_unlock(&w.mu);
	}
}

static void do_op(int op, int on, const char *ssid, const char *psk)
{
	char cmd[300], q[80];
	switch (op) {
	case OP_POWER:
		if (on) {
			run("rfkill unblock wifi; ifconfig wlan0 up; pidof wpa_supplicant > /dev/null || "
			    "wpa_supplicant -B -Dnl80211 -iwlan0 -c" CONF " > /dev/null 2>&1", NULL, NULL);
			usleep(500000);
			run(CLI "scan > /dev/null 2>&1", NULL, NULL);
		} else {
			run("killall udhcpc wpa_supplicant > /dev/null 2>&1; ifconfig wlan0 down", NULL, NULL);
			pthread_mutex_lock(&w.mu);
			w.online = 0;
			w.nnet = 0;
			w.gen++;
			pthread_mutex_unlock(&w.mu);
		}
		w.power = on;
		break;
	case OP_SCAN:
		set_status("поиск сетей…");
		run(CLI "scan > /dev/null 2>&1", NULL, NULL);
		sleep(4);
		set_status("");
		break;
	case OP_CONNECT: {
		set_status("подключение…");
		char qa[80];
		quoted(q, sizeof(q), ssid);
		quoted_as(qa, sizeof(qa), ssid, 0);
		int id = -1;
		FILE *f;
		snprintf(cmd, sizeof(cmd), CLI "list_networks | awk -F'\\t' -v s=%s '$2==s{print $1; exit}'", qa);
		if (psk && (f = sp_popen(CLI "add_network", "r"))) {   /* новая сеть: добавить и сохранить в конфиг */
			if (fscanf(f, "%d", &id) != 1)
				id = -1;
			sp_pclose(f);
			if (id >= 0) {
				char qp[140];
				quoted(qp, sizeof(qp), psk);
				snprintf(cmd, sizeof(cmd), CLI "set_network %d ssid %s > /dev/null", id, q);
				run(cmd, NULL, NULL);
				if (psk[0])
					snprintf(cmd, sizeof(cmd), CLI "set_network %d psk %s > /dev/null", id, qp);
				else
					snprintf(cmd, sizeof(cmd), CLI "set_network %d key_mgmt NONE > /dev/null", id);
				run(cmd, NULL, NULL);
			}
		} else if ((f = sp_popen(cmd, "r"))) {   /* сохранённая */
			if (fscanf(f, "%d", &id) != 1)
				id = -1;
			sp_pclose(f);
		}
		if (id < 0) {
			set_status("");
			set_msg("Не удалось подключить");
			break;
		}
		snprintf(cmd, sizeof(cmd), CLI "select_network %d > /dev/null; " CLI "save_config > /dev/null", id);
		run(cmd, NULL, NULL);
		pthread_mutex_lock(&w.mu);
		w.online = 0;
		pthread_mutex_unlock(&w.mu);
		st_t s = { "", "", "" };
		for (int i = 0; i < 15 && strcmp(s.state, "COMPLETED"); i++) {
			sleep(1);
			run(CLI "status 2>/dev/null", status_line, &s);
		}
		set_status("");
		if (!strcmp(s.state, "COMPLETED")) {
			run(CLI "enable_network all > /dev/null", NULL, NULL);   /* select выключил остальные — вернуть */
			run(CLI "save_config > /dev/null", NULL, NULL);
			bring_up(s.ssid);
		} else {
			set_msg(psk ? "Не подключилось — неверный пароль?" : "Не удалось подключить");
			run(CLI "enable_network all > /dev/null", NULL, NULL);
		}
		break;
	}
	case OP_FORGET:
		quoted_as(q, sizeof(q), ssid, 0);
		snprintf(cmd, sizeof(cmd),
			 "id=$(" CLI "list_networks | awk -F'\\t' -v s=%s '$2==s{print $1; exit}'); [ -n \"$id\" ] && "
			 CLI "remove_network $id > /dev/null && " CLI "save_config > /dev/null", q);
		run(cmd, NULL, NULL);
		set_msg("Сеть забыта");
		break;
	}
	w.refresh = 1;
}

static void *wifi_thread(void *arg)
{
	(void)arg;
	struct sched_param sp = { 0 };
	pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp);
	for (;;) {
		pthread_mutex_lock(&w.mu);
		if (!w.op) {
			struct timespec ts;
			clock_gettime(CLOCK_REALTIME, &ts);
			ts.tv_sec += 3;
			pthread_cond_timedwait(&w.cv, &w.mu, &ts);
		}
		int op = w.op, on = w.op_on;
		char ssid[33], psk[64];
		snprintf(ssid, sizeof(ssid), "%s", w.op_ssid);
		snprintf(psk, sizeof(psk), "%s", w.op_psk);
		int has_psk = w.op_psk[0] != 1;   /* 1 — пароля нет (сохранённая сеть) */
		w.op = OP_NONE;
		pthread_mutex_unlock(&w.mu);
		if (op)
			do_op(op, on, ssid, has_psk ? psk : NULL);
		if (w.power) {
			poll_state();
			if (w.refresh) {
				w.refresh = 0;
				refresh_list();
			}
		}
	}
	return NULL;
}

static void post(int op, int on, const char *ssid, const char *psk)
{
	pthread_mutex_lock(&w.mu);
	w.op = op;
	w.op_on = on;
	snprintf(w.op_ssid, sizeof(w.op_ssid), "%s", ssid ? ssid : "");
	if (psk)
		snprintf(w.op_psk, sizeof(w.op_psk), "%s", psk);
	else
		w.op_psk[0] = 1, w.op_psk[1] = 0;
	pthread_cond_signal(&w.cv);
	pthread_mutex_unlock(&w.mu);
}

void wifi_init(int on)
{
	pthread_t t;
	w.op = OP_POWER;
	w.op_on = on;
	pthread_create(&t, NULL, wifi_thread, NULL);
	pthread_detach(t);
}
#else
static void post(int op, int on, const char *ssid, const char *psk)
{
	(void)ssid;
	(void)psk;
	w.power = op == OP_POWER ? on : w.power;
}

void wifi_init(int on)
{
	w.power = on;
}
#endif

void wifi_power(int on)
{
	post(OP_POWER, on, NULL, NULL);
}

void wifi_scan(void)
{
	post(OP_SCAN, 0, NULL, NULL);
}

void wifi_connect(const char *ssid, const char *psk)
{
	post(OP_CONNECT, 0, ssid, psk);
}

void wifi_forget(const char *ssid)
{
	post(OP_FORGET, 0, ssid, NULL);
}

int wifi_networks(wifi_net_t *out, int max)
{
	pthread_mutex_lock(&w.mu);
	int n = w.nnet < max ? w.nnet : max;
	memcpy(out, w.net, sizeof(wifi_net_t) * n);
	pthread_mutex_unlock(&w.mu);
	return n;
}

int wifi_online(void)
{
	return w.online;
}

unsigned wifi_gen(void)
{
	return w.gen;
}

const char *wifi_status(void)
{
	return w.status;
}

const char *wifi_message(unsigned *seq)
{
	*seq = w.msg_seq;
	return w.msg;
}
