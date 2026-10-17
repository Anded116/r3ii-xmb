/* lyrics.c — разбор LRC: у строки бывает несколько меток ([00:12.30][01:40.00]припев), метки [ar:], [ti:] и т. п.
 * пропускаются, [offset:+/-мс] сдвигает всё. Нет ни одной метки времени — простой текст. */
#include "lyrics.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
