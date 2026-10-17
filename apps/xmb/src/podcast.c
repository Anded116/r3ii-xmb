/* podcast.c — подписки из файлов на карте, RSS и выпуски — xmbget (HTTPS через mbedTLS) в фоновом потоке, на
 * пониженном приоритете: звук не страдает; свой разбор RSS без XML-библиотеки: каналу нужны
 * <title>, картинка (itunes:image или <image><url>), у выпусков — <title>, <pubDate>, <enclosure url type>. */
#define _GNU_SOURCE
#include "podcast.h"

#include "filesave.h"
#include "i18n.h"
#include "spawn.h"

#include <ctype.h>
#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CHECK_EVERY (6 * 3600)   /* секунд между проверками, пока есть Wi-Fi */
#define MAX_FEEDS 64
#define RSS_MAX (4 * 1024 * 1024)

typedef struct {
	char *path;
	double pos;
	int done;
} st_t;

static struct {
	pthread_mutex_t mu;
	char dir[320], state_file[340];
	st_t *st;
	int nst;
	int busy, want, nfeeds, new_files;
	time_t last;
	unsigned msg_seq;
	char status[96], msg[96];
	char part[920];          /* выпуск, который качается сейчас; пусто — не качается */
	long long part_len;      /* его размер из RSS, 0 — неизвестен */
} pd = { .mu = PTHREAD_MUTEX_INITIALIZER };

/* ---------- состояние выпусков ---------- */

static st_t *st_find(const char *path)
{
	for (int i = 0; i < pd.nst; i++)
		if (!strcmp(pd.st[i].path, path))
			return &pd.st[i];
	return NULL;
}

static void st_write(void)   /* под pd.mu */
{
	size_t cap = 64, len = 0;
	for (int i = 0; i < pd.nst; i++)
		cap += strlen(pd.st[i].path) + 32;
	char *buf = malloc(cap);
	for (int i = 0; i < pd.nst; i++)
		len += (size_t)snprintf(buf + len, cap - len, "%d\t%.1f\t%s\n", pd.st[i].done, pd.st[i].pos, pd.st[i].path);
	fsave(pd.state_file, buf, len);
	free(buf);
}

static void st_load(void)
{
	FILE *f = fopen(pd.state_file, "r");
	char line[1200];
	while (f && fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		int done;
		double pos;
		int off = 0;
		if (sscanf(line, "%d\t%lf\t%n", &done, &pos, &off) < 2 || !off)
			continue;
		pd.st = realloc(pd.st, sizeof(st_t) * (pd.nst + 1));
		pd.st[pd.nst++] = (st_t){ strdup(line + off), pos, done };
	}
	if (f)
		fclose(f);
}

double pod_pos(const char *path)
{
	pthread_mutex_lock(&pd.mu);
	st_t *s = st_find(path);
	double p = s && !s->done ? s->pos : 0;
	pthread_mutex_unlock(&pd.mu);
	return p;
}

int pod_done(const char *path)
{
	pthread_mutex_lock(&pd.mu);
	st_t *s = st_find(path);
	int d = s && s->done;
	pthread_mutex_unlock(&pd.mu);
	return d;
}

void pod_save(const char *path, double pos, int done)
{
	pthread_mutex_lock(&pd.mu);
	st_t *s = st_find(path);
	if (!s) {
		pd.st = realloc(pd.st, sizeof(st_t) * (pd.nst + 1));
		s = &pd.st[pd.nst++];
		*s = (st_t){ strdup(path), 0, 0 };
	}
	if (s->done != done || (int)s->pos != (int)pos) {
		s->pos = pos;
		s->done = done;
		st_write();
	}
	pthread_mutex_unlock(&pd.mu);
}

/* ---------- подписки ---------- */

/* ссылки из подписки.txt и всех .opml (xmlUrl="…"); без повторов */
static int read_feeds(char urls[][512], int max)
{
	int n = 0;
	char f[400], line[1024];
	snprintf(f, sizeof(f), "%s/подписки.txt", pd.dir);
	FILE *fp = fopen(f, "r");
	while (fp && fgets(line, sizeof(line), fp) && n < max) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		p[strcspn(p, " \t\r\n")] = 0;
		if (!strncmp(p, "http", 4))
			snprintf(urls[n++], 512, "%s", p);
	}
	if (fp)
		fclose(fp);
	DIR *d = opendir(pd.dir);
	struct dirent *e;
	while (d && (e = readdir(d)) && n < max) {
		const char *dot = strrchr(e->d_name, '.');
		if (!dot || strcasecmp(dot, ".opml"))
			continue;
		snprintf(f, sizeof(f), "%s/%s", pd.dir, e->d_name);
		FILE *o = fopen(f, "r");
		static char buf[256 * 1024];
		size_t len = o ? fread(buf, 1, sizeof(buf) - 1, o) : 0;
		if (o)
			fclose(o);
		buf[len] = 0;
		for (char *p = buf; (p = strstr(p, "xmlUrl=\"")) && n < max; p++) {
			p += 8;
			char *q = strchr(p, '"');
			if (q && q - p < 511)
				snprintf(urls[n++], 512, "%.*s", (int)(q - p), p);
		}
	}
	if (d)
		closedir(d);
	for (int i = 0; i < n; i++)   /* без повторов */
		for (int j = i + 1; j < n; j++)
			if (!strcmp(urls[i], urls[j]))
				memmove(urls[j], urls[j + 1], (size_t)(--n - j) * 512), j--;
	return n;
}

/* ---------- RSS ---------- */

static void entities(char *s)
{
	static const struct { const char *e; char c; } map[] = {
		{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' }, { "&#39;", '\'' },
	};
	char *o = s;
	for (char *p = s; *p;) {
		int hit = 0;
		for (unsigned k = 0; k < sizeof(map) / sizeof(map[0]) && !hit; k++)
			if (!strncmp(p, map[k].e, strlen(map[k].e))) {
				*o++ = map[k].c;
				p += strlen(map[k].e);
				hit = 1;
			}
		if (!hit && p[0] == '&' && p[1] == '#') {   /* &#NNN; и &#xHH; — в UTF-8 */
			char *end;
			unsigned c = p[2] == 'x' ? (unsigned)strtoul(p + 3, &end, 16) : (unsigned)strtoul(p + 2, &end, 10);
			if (*end == ';' && c) {
				if (c < 0x80) {
					*o++ = (char)c;
				} else if (c < 0x800) {
					*o++ = (char)(0xc0 | c >> 6);
					*o++ = (char)(0x80 | (c & 0x3f));
				} else {
					*o++ = (char)(0xe0 | (c >> 12 & 0x0f));
					*o++ = (char)(0x80 | ((c >> 6) & 0x3f));
					*o++ = (char)(0x80 | (c & 0x3f));
				}
				p = end + 1;
				hit = 1;
			}
		}
		if (!hit)
			*o++ = *p++;
	}
	*o = 0;
}

/* текст первого <tag>…</tag> в [s, end): CDATA снимается, сущности раскрываются, пробелы по краям — прочь */
static int tag_text(const char *s, const char *end, const char *tag, char *out, int cap)
{
	char open[64], close[64];
	snprintf(open, sizeof(open), "<%s", tag);
	snprintf(close, sizeof(close), "</%s>", tag);
	const char *p = s;
	while ((p = strstr(p, open)) && p < end && p[strlen(open)] != '>' && !isspace((unsigned char)p[strlen(open)]))
		p++;
	if (!p || p >= end)
		return 0;
	p = strchr(p, '>');
	const char *q = p ? strstr(p, close) : NULL;
	if (!q || q > end)
		return 0;
	p++;
	while (p < q && isspace((unsigned char)*p))
		p++;
	if (!strncmp(p, "<![CDATA[", 9)) {
		p += 9;
		const char *c = strstr(p, "]]>");
		if (c && c < q)
			q = c;
	}
	int n = (int)(q - p) < cap - 1 ? (int)(q - p) : cap - 1;
	memcpy(out, p, n);
	out[n] = 0;
	entities(out);
	while (n && isspace((unsigned char)out[n - 1]))
		out[--n] = 0;
	return n > 0;
}

/* значение атрибута у первого <tag …> в [s, end) */
static int tag_attr(const char *s, const char *end, const char *tag, const char *attr, char *out, int cap)
{
	char open[64], key[64];
	snprintf(open, sizeof(open), "<%s", tag);
	snprintf(key, sizeof(key), "%s=", attr);
	const char *p = strstr(s, open);
	if (!p || p >= end)
		return 0;
	const char *gt = strchr(p, '>');
	const char *a = strstr(p, key);
	if (!a || !gt || a > gt)
		return 0;
	a += strlen(key);
	char qc = *a;
	if (qc != '"' && qc != '\'')
		return 0;
	const char *q = strchr(a + 1, qc);
	if (!q || q > gt)
		return 0;
	int n = (int)(q - a - 1) < cap - 1 ? (int)(q - a - 1) : cap - 1;
	memcpy(out, a + 1, n);
	out[n] = 0;
	entities(out);
	return n > 0;
}

/* «Wed, 15 Oct 2026 10:00:00 +0000» → «2026-10-15» */
static void rss_date(const char *s, char *out)
{
	static const char *const mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
	const char *p = strchr(s, ',');
	p = p ? p + 1 : s;
	int d = 0, y = 0, m = 0;
	char mn[8] = "";
	if (sscanf(p, "%d %3s %d", &d, mn, &y) == 3)
		for (int i = 0; i < 12; i++)
			if (!strcasecmp(mn, mon[i]))
				m = i + 1;
	if (m)
		sprintf(out, "%04d-%02d-%02d", y, m, d);
	else
		strcpy(out, "0000-00-00");
}

/* имя файла: без символов, запрещённых на exFAT, не длиннее cap (не посреди буквы UTF-8) */
static void safe_name(const char *s, char *out, int cap)
{
	int k = 0;
	for (; *s && k < cap - 1; s++)
		out[k++] = strchr("/\\:*?\"<>|\t\r\n", *s) ? ' ' : *s;
	while (k && ((unsigned char)out[k - 1] & 0xc0) == 0x80)   /* обрезали посреди буквы — убрать хвост */
		k--;
	if (k && (unsigned char)out[k - 1] >= 0xc0)
		k--;
	out[k] = 0;
	while (k && (out[k - 1] == ' ' || out[k - 1] == '.'))
		out[--k] = 0;
	char *p = out;
	while (*p == ' ' || *p == '.')
		p++;
	memmove(out, p, strlen(p) + 1);
	if (!out[0])
		strcpy(out, "_");
}

/* строка для shell в одинарных кавычках */
static void sq(char *out, int cap, const char *s)
{
	int k = 0;
	out[k++] = '\'';
	for (; *s && k < cap - 6; s++) {
		if (*s == '\'') {
			memcpy(out + k, "'\\''", 4);
			k += 4;
		} else {
			out[k++] = *s;
		}
	}
	out[k++] = '\'';
	out[k] = 0;
}

static int fetch(const char *url, const char *file, int timeout)
{
	char cmd[3500], qu[1100], qf[1100];
	sq(qu, sizeof(qu), url);
	sq(qf, sizeof(qf), file);
	const char *get = sp_xmbget();
	if (!get)
		return -1;
	snprintf(cmd, sizeof(cmd), "nice -n 19 %s -T %d -O %s %s 2>>/tmp/xmb_get.log", get, timeout, qf, qu);
	/* Wi-Fi плеера временами теряет DNS и пакеты: ещё две попытки; «сервер ответил не 200» (код 2) не повторять */
	int rc = -1;
	for (int i = 0; i < 3; i++) {
		rc = sp_system(cmd);
		if (rc == 0 || rc == (2 << 8))
			break;
		sleep(3 + 3 * i);
	}
	return rc;
}

static void set_status(const char *fmt, int a, int b)
{
	pthread_mutex_lock(&pd.mu);
	snprintf(pd.status, sizeof(pd.status), tx(fmt), a, b);   /* на языке интерфейса */
	pthread_mutex_unlock(&pd.mu);
}

static int is_jpeg(const char *file)
{
	unsigned char m[3] = { 0 };
	FILE *f = fopen(file, "rb");
	if (f) {
		if (fread(m, 1, 3, f) != 3)
			m[0] = 0;
		fclose(f);
	}
	return m[0] == 0xff && m[1] == 0xd8 && m[2] == 0xff;
}

static int name_desc(const void *a, const void *b)
{
	return strcmp(*(char *const *)b, *(char *const *)a);
}

/* сверх POD_KEEP свежих — удалить прослушанные (имена начинаются с даты: по имени — по свежести) */
static void cleanup(const char *dir)
{
	DIR *d = opendir(dir);
	if (!d)
		return;
	char *names[256];
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d)) && n < 256) {
		const char *dot = strrchr(e->d_name, '.');
		if (e->d_name[0] != '.' && dot && (!strcasecmp(dot, ".mp3") || !strcasecmp(dot, ".m4a")))
			names[n++] = strdup(e->d_name);
	}
	closedir(d);
	qsort(names, n, sizeof(char *), name_desc);
	for (int i = 0; i < n; i++) {
		char f[1200];
		snprintf(f, sizeof(f), "%s/%s", dir, names[i]);
		if (i >= POD_KEEP && pod_done(f))
			unlink(f);
		free(names[i]);
	}
}

/* один канал: свежие выпуски — в его папку; 1 — канал прочитан */
static int do_feed(const char *url, int fi, int nf, int *got)
{
	const char *rss = "/tmp/xmb_rss.xml";
	unlink(rss);
	set_status("проверка подписок: %d из %d", fi + 1, nf);
	fetch(url, rss, 25);
	FILE *f = fopen(rss, "rb");
	if (!f)
		return 0;
	char *x = malloc(RSS_MAX + 1);
	size_t len = fread(x, 1, RSS_MAX, f);
	fclose(f);
	unlink(rss);
	x[len] = 0;
	char *end = x + len, *first = strstr(x, "<item");
	char title[256], img[1024] = "", name[96];
	if (!tag_text(x, first ? first : end, "title", title, sizeof(title))) {
		free(x);
		return 0;
	}
	if (!tag_attr(x, first ? first : end, "itunes:image", "href", img, sizeof(img))) {
		char *im = strstr(x, "<image>");
		if (im && (!first || im < first))
			tag_text(im, first ? first : end, "url", img, sizeof(img));
	}
	safe_name(title, name, sizeof(name));
	char dir[600];
	snprintf(dir, sizeof(dir), "%s/%s", pd.dir, name);
	mkdir(dir, 0755);
	char cover[700];
	snprintf(cover, sizeof(cover), "%s/cover.jpg", dir);
	struct stat st;
	if (img[0] && stat(cover, &st)) {
		fetch(img, cover, 25);
		if (!is_jpeg(cover))   /* PNG и прочее обложки не покажут: тогда без неё */
			unlink(cover);
	}
	int taken = 0;
	for (char *it = first; it && taken < POD_KEEP; it = strstr(it + 5, "<item")) {
		char *ie = strstr(it, "</item>");
		if (!ie)
			break;
		char t[256] = "", date[32] = "", enc[1024] = "", type[64] = "", pub[96] = "", len[24] = "";
		tag_text(it, ie, "title", t, sizeof(t));
		tag_text(it, ie, "pubDate", pub, sizeof(pub));
		tag_attr(it, ie, "enclosure", "url", enc, sizeof(enc));
		tag_attr(it, ie, "enclosure", "type", type, sizeof(type));
		tag_attr(it, ie, "enclosure", "length", len, sizeof(len));
		if (!enc[0])
			continue;
		char path_part[1024];
		snprintf(path_part, sizeof(path_part), "%.*s", (int)strcspn(enc, "?#"), enc);
		const char *dot = strrchr(path_part, '.');
		const char *ext = strstr(type, "mpeg") || (dot && !strcasecmp(dot, ".mp3")) ? "mp3"
				  : strstr(type, "mp4") || strstr(type, "m4a") || strstr(type, "aac") || (dot && !strcasecmp(dot, ".m4a"))
				  ? "m4a" : NULL;
		if (!ext)
			continue;   /* видео и форматы, которых плеер не играет */
		taken++;
		rss_date(pub, date);
		char ep[200], file[900], part[920];
		safe_name(t[0] ? t : "выпуск", ep, 150);
		snprintf(file, sizeof(file), "%s/%s - %s.%s", dir, date, ep, ext);
		if (!stat(file, &st))
			continue;
		snprintf(part, sizeof(part), "%s.part", file);
		set_status("скачивается выпуск %d (подписка %d)", taken, fi + 1);
		pthread_mutex_lock(&pd.mu);
		snprintf(pd.part, sizeof(pd.part), "%s", part);
		pd.part_len = atoll(len);
		pthread_mutex_unlock(&pd.mu);
		int rc = fetch(enc, part, 60);
		pthread_mutex_lock(&pd.mu);
		pd.part[0] = 0;
		pthread_mutex_unlock(&pd.mu);
		if (rc == 0 && !stat(part, &st) && st.st_size > 0 && !rename(part, file)) {
			(*got)++;
			printf("podcast: %s\n", file);
			fflush(stdout);
		} else {
			unlink(part);
		}
	}
	free(x);
	cleanup(dir);
	return 1;
}

static void *pod_thread(void *arg)
{
	(void)arg;
	static char urls[MAX_FEEDS][512];
	int n = read_feeds(urls, MAX_FEEDS), got = 0, ok = 0;
	pd.nfeeds = n;
	for (int i = 0; i < n; i++)
		ok += do_feed(urls[i], i, n, &got);
	fsave_sync();
	pthread_mutex_lock(&pd.mu);
	pd.status[0] = 0;
	pd.new_files += got;
	if (got)
		snprintf(pd.msg, sizeof(pd.msg), tx("Подкасты: новых выпусков — %d"), got);
	else if (n && !ok)
		snprintf(pd.msg, sizeof(pd.msg), "%s", tx("Подкасты: подписки не открылись"));
	else
		pd.msg[0] = 0;
	if (pd.msg[0])
		pd.msg_seq++;
	pd.busy = 0;
	pthread_mutex_unlock(&pd.mu);
	printf("podcast: подписок %d, прочитано %d, новых выпусков %d\n", n, ok, got);
	fflush(stdout);
	return NULL;
}

/* ---------- API ---------- */

void pod_init(const char *music_root)
{
	snprintf(pd.dir, sizeof(pd.dir), "%s/Podcasts", music_root ? music_root : ".");
	snprintf(pd.state_file, sizeof(pd.state_file), "%s/.state", pd.dir);
	st_load();
	char urls[4][512];
	pd.nfeeds = read_feeds(urls, 4);
}

void pod_update(void)
{
	pd.want = 1;
}

void pod_tick(int online)
{
	if (!online || pd.busy)
		return;
	time_t now = time(NULL);
	if (!pd.want && pd.last && now - pd.last < CHECK_EVERY)
		return;
	pd.want = 0;
	pd.last = now;
	mkdir(pd.dir, 0755);
	pd.busy = 1;
	pthread_t t;
	pthread_create(&t, NULL, pod_thread, NULL);
	pthread_detach(t);
}

int pod_busy(void)
{
	return pd.busy;
}

const char *pod_status(void)
{
	return pd.status;
}

double pod_progress(void)
{
	double r = -1;
	struct stat st;
	pthread_mutex_lock(&pd.mu);
	if (pd.part[0] && pd.part_len > 0)
		r = !stat(pd.part, &st) ? (double)st.st_size / pd.part_len : 0;
	pthread_mutex_unlock(&pd.mu);
	return r > 1 ? 1 : r;
}

const char *pod_message(unsigned *seq)
{
	*seq = pd.msg_seq;
	return pd.msg;
}

int pod_take_new(void)
{
	pthread_mutex_lock(&pd.mu);
	int n = pd.new_files;
	pd.new_files = 0;
	pthread_mutex_unlock(&pd.mu);
	return n;
}

int pod_feeds(void)
{
	return pd.nfeeds;
}
