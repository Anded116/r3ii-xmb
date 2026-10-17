/* lyrics.c — разбор LRC: у строки бывает несколько меток ([00:12.30][01:40.00]припев), метки [ar:], [ti:] и т. п.
 * пропускаются, [offset:+/-мс] сдвигает всё. Нет ни одной метки времени — простой текст. */
#include "lyrics.h"
#include "spawn.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void lyrics_path(char *out, int cap, const char *audio_path, int cue, const char *artist, const char *title)
{
	const char *slash = strrchr(audio_path, '/');
	if (cue) {
		int dir = slash ? (int)(slash - audio_path) : 0;
		snprintf(out, cap, "%.*s/%s - %s.lrc", dir, audio_path, artist, title);
		for (char *c = out + dir + 1; *c; c++)   /* символы, которых не бывает в именах файлов */
			if (strchr("/\\:*?\"<>|", *c))
				*c = '_';
		return;
	}
	const char *dot = strrchr(audio_path, '.');
	int n = dot && (!slash || dot > slash) ? (int)(dot - audio_path) : (int)strlen(audio_path);
	snprintf(out, cap, "%.*s.lrc", n, audio_path);
}

void lyrics_free(lyrics_t *l)
{
	free(l->v);
	free(l->buf);
	memset(l, 0, sizeof(*l));
}

/* «[мм:сс.сс]» с позиции p: секунды, p — за скобку; не метка времени — -1 */
static double stamp(const char **p)
{
	const char *s = *p;
	if (*s != '[' || !isdigit((unsigned char)s[1]))
		return -1;
	char *e;
	long m = strtol(s + 1, &e, 10);
	if (*e != ':')
		return -1;
	double sec = strtod(e + 1, &e);
	if (*e != ']')
		return -1;
	*p = e + 1;
	return m * 60 + sec;
}

static int cmp_t(const void *a, const void *b)
{
	double x = ((const lyric_t *)a)->t, y = ((const lyric_t *)b)->t;
	return x < y ? -1 : x > y;
}

int lyrics_parse(lyrics_t *l, const char *text)
{
	lyrics_free(l);
	l->buf = strdup(text);
	if (!l->buf)
		return 0;
	int cap = 64;
	l->v = malloc(sizeof(lyric_t) * cap);
	double offset = 0;
	char *line = l->buf;
	while (line && *line) {
		char *next = strchr(line, '\n');
		if (next)
			*next++ = 0;
		line[strcspn(line, "\r")] = 0;
		if (!strncmp(line, "\xEF\xBB\xBF", 3))
			line += 3;
		const char *p = line;
		double ts[16];
		int nt = 0;
		for (double t; nt < 16 && (t = stamp(&p)) >= 0;)
			ts[nt++] = t;
		if (!nt && line[0] == '[' && strchr(line, ']') && !strchr(line, ']')[1]) {   /* [ar:…], [offset:…] */
			if (!strncmp(line, "[offset:", 8))
				offset = atof(line + 8) / 1000.0;
			line = next;
			continue;
		}
		while (*p == ' ')
			p++;
		for (int i = 0; i < (nt ? nt : 1); i++) {
			if (l->n == cap)
				l->v = realloc(l->v, sizeof(lyric_t) * (cap *= 2));
			l->v[l->n++] = (lyric_t){ nt ? ts[i] : -1, p };
		}
		l->synced |= nt > 0;
		line = next;
	}
	if (l->synced) {   /* строки без меток среди LRC — мусор */
		int k = 0;
		for (int i = 0; i < l->n; i++)
			if (l->v[i].t >= 0)
				l->v[k++] = (lyric_t){ l->v[i].t - offset, l->v[i].text };
		l->n = k;
		qsort(l->v, l->n, sizeof(lyric_t), cmp_t);
	} else {
		while (l->n && !l->v[l->n - 1].text[0])   /* пустые строки в конце */
			l->n--;
		for (int i = 0; i < l->n; i++)
			l->v[i].t = 0;
	}
	if (!l->n)
		lyrics_free(l);
	return l->n;
}

int lyrics_load(lyrics_t *l, const char *lrc_path)
{
	FILE *f = fopen(lrc_path, "rb");
	if (!f) {
		lyrics_free(l);
		return 0;
	}
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (sz <= 0 || sz > 256 * 1024) {
		fclose(f);
		lyrics_free(l);
		return 0;
	}
	char *t = malloc(sz + 1);
	size_t got = fread(t, 1, sz, f);
	fclose(f);
	t[got] = 0;
	int n = lyrics_parse(l, t);
	free(t);
	return n;
}

int lyrics_at(const lyrics_t *l, double sec)
{
	int lo = 0, hi = l->n - 1, r = -1;
	while (lo <= hi) {
		int m = (lo + hi) / 2;
		if (l->v[m].t <= sec) {
			r = m;
			lo = m + 1;
		} else {
			hi = m - 1;
		}
	}
	return r;
}

/* ---------- LRCLIB ---------- */

#include <pthread.h>

static struct {
	pthread_mutex_t mu;
	int state;
	char url[2][1024], path[1100];
} lf = { .mu = PTHREAD_MUTEX_INITIALIZER };

static void url_add(char *u, int cap, const char *key, const char *val)
{
	int k = (int)strlen(u);
	k += snprintf(u + k, cap - k, "%s%s=", strchr(u, '?') ? "&" : "?", key);
	for (const unsigned char *p = (const unsigned char *)val; *p && k < cap - 4; p++) {
		if (isalnum(*p) || strchr("-_.~", *p))
			u[k++] = (char)*p;
		else
			k += snprintf(u + k, cap - k, "%%%02X", *p);
	}
	u[k] = 0;
}

/* строковое поле JSON с позиции p (после ключа и двоеточия): разэкранировать в out; null или не строка — 0 */
static int json_str(const char *p, char *out, int cap)
{
	while (*p == ' ')
		p++;
	if (*p != '"')
		return 0;
	int k = 0;
	for (p++; *p && *p != '"' && k < cap - 5; p++) {
		if (*p != '\\') {
			out[k++] = *p;
			continue;
		}
		p++;
		switch (*p) {
		case 'n': out[k++] = '\n'; break;
		case 't': out[k++] = '\t'; break;
		case 'r': break;
		case 'u': {
			unsigned c = (unsigned)strtoul((char[]){ p[1], p[2], p[3], p[4], 0 }, NULL, 16);
			p += 4;
			if (c >= 0xd800 && c < 0xdc00 && p[1] == '\\' && p[2] == 'u') {   /* суррогатная пара */
				unsigned lo = (unsigned)strtoul((char[]){ p[3], p[4], p[5], p[6], 0 }, NULL, 16);
				c = 0x10000 + ((c - 0xd800) << 10) + (lo - 0xdc00);
				p += 6;
			}
			if (c < 0x80) {
				out[k++] = (char)c;
			} else if (c < 0x800) {
				out[k++] = (char)(0xc0 | c >> 6);
				out[k++] = (char)(0x80 | (c & 0x3f));
			} else if (c < 0x10000) {
				out[k++] = (char)(0xe0 | c >> 12);
				out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
				out[k++] = (char)(0x80 | (c & 0x3f));
			} else {
				out[k++] = (char)(0xf0 | c >> 18);
				out[k++] = (char)(0x80 | ((c >> 12) & 0x3f));
				out[k++] = (char)(0x80 | ((c >> 6) & 0x3f));
				out[k++] = (char)(0x80 | (c & 0x3f));
			}
			break;
		}
		default: out[k++] = *p; break;   /* \" \\ \/ */
		}
	}
	out[k] = 0;
	return k > 0 && *p == '"';   /* ответ оборвался посреди строки — не брать */
}

/* первый непустой текст в ответе: синхронный важнее простого (у поиска — массив, берём первый с метками) */
static int pick(const char *json, char *out, int cap)
{
	static const char *const keys[2] = { "\"syncedLyrics\":", "\"plainLyrics\":" };
	for (int i = 0; i < 2; i++)
		for (const char *p = json; (p = strstr(p, keys[i])); p++)
			if (json_str(p + strlen(keys[i]), out, cap))
				return 1;
	return 0;
}

#if XMB_DEVICE
static int get(const char *url, const char *file)
{
	char cmd[1300];
	const char *get = sp_xmbget();
	if (!get)
		return -1;
	snprintf(cmd, sizeof(cmd), "%s -T 20 -O %s '%s' 2>>/tmp/xmb_get.log", get, file, url);
	return sp_system(cmd);
}
#else
static int get(const char *url, const char *file)
{
	(void)url;
	(void)file;
	return -1;
}
#endif

static void *fetch_thread(void *arg)
{
	(void)arg;
	const char *tmp = "/tmp/xmb_lrclib.json";
	int st = LF_ERROR;
	char *text = malloc(256 * 1024);
	/* точный запрос по длительности, не нашлось — поиск; сервер не ответил — точный ещё раз */
	for (int a = 0; a < 3 && text && (a < 2 || st == LF_ERROR); a++) {
		int i = a % 2;
		if (a == 2)
			sleep(2);
		unlink(tmp);
		int rc = get(lf.url[i], tmp);
		FILE *f = fopen(tmp, "rb");
		if (!f) {
			st = rc == (2 << 8) ? LF_NONE : LF_ERROR;   /* xmbget: 2 — сервер ответил не 200 (нет текста) */
			continue;
		}
		static char json[512 * 1024];
		size_t n = fread(json, 1, sizeof(json) - 1, f);
		fclose(f);
		json[n] = 0;
		st = LF_NONE;
		if (!pick(json, text, 256 * 1024))
			continue;
		lyrics_t l = { 0 };
		if (lyrics_parse(&l, text) && (f = fopen(lf.path, "wb"))) {
			fputs(text, f);
			fputc('\n', f);
			fclose(f);
			st = LF_FOUND;
		}
		lyrics_free(&l);
		if (st == LF_FOUND)
			break;
	}
	free(text);
	unlink(tmp);
	printf("lyrics: LRCLIB — %s\n", st == LF_FOUND ? "найден" : st == LF_NONE ? "нет" : "ошибка сети");
	fflush(stdout);
	pthread_mutex_lock(&lf.mu);
	lf.state = st;
	pthread_mutex_unlock(&lf.mu);
	return NULL;
}

void lyrics_fetch(const char *artist, const char *title, const char *album, double dur, const char *lrc_path)
{
	pthread_mutex_lock(&lf.mu);
	if (lf.state == LF_BUSY) {
		pthread_mutex_unlock(&lf.mu);
		return;
	}
	lf.state = LF_BUSY;
	pthread_mutex_unlock(&lf.mu);
	char d[16];
	snprintf(d, sizeof(d), "%d", (int)(dur + 0.5));
	const char *t = title;   /* «6. Название», «06 - Название» — номер из тегов LRCLIB не знает */
	while (isdigit((unsigned char)*t))
		t++;
	if (t > title && t - title <= 3 && strchr(".-) ", *t)) {
		while (*t && strchr(".-) ", *t))
			t++;
		title = *t ? t : title;
	}
	snprintf(lf.url[0], sizeof(lf.url[0]), "https://lrclib.net/api/get");
	url_add(lf.url[0], sizeof(lf.url[0]), "artist_name", artist);
	url_add(lf.url[0], sizeof(lf.url[0]), "track_name", title);
	if (album && *album)
		url_add(lf.url[0], sizeof(lf.url[0]), "album_name", album);
	url_add(lf.url[0], sizeof(lf.url[0]), "duration", d);
	snprintf(lf.url[1], sizeof(lf.url[1]), "https://lrclib.net/api/search");
	url_add(lf.url[1], sizeof(lf.url[1]), "track_name", title);
	url_add(lf.url[1], sizeof(lf.url[1]), "artist_name", artist);
	snprintf(lf.path, sizeof(lf.path), "%s", lrc_path);
	pthread_t th;
	pthread_create(&th, NULL, fetch_thread, NULL);
	pthread_detach(th);
}

int lyrics_fetch_poll(void)
{
	pthread_mutex_lock(&lf.mu);
	int st = lf.state;
	if (st != LF_BUSY)
		lf.state = LF_IDLE;
	pthread_mutex_unlock(&lf.mu);
	return st;
}
