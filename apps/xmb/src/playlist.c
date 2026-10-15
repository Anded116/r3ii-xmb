/*
 * Плейлисты .m3u8: чтение (пути абсолютные или относительно папки плейлиста, разделители / и \),
 * запись — относительными путями через временный файл, чтобы выдернутая карта не оставила половину.
 */
#include "playlist.h"

#include "filesave.h"

#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define FAV_NAME "Избранное"

static const library_t *lib;
static int *by_path;   /* индексы треков, отсортированные по (путь, начало) */

static int cmp_path(const void *a, const void *b)
{
	const lib_track_t *x = &lib->tracks[*(const int *)a], *y = &lib->tracks[*(const int *)b];
	int c = strcmp(x->path, y->path);
	return c ? c : x->start < y->start ? -1 : x->start > y->start;
}

/* трек по пути и началу (у CUE несколько треков на один файл): ближайший по началу */
static int find_track(const char *path, double start)
{
	int lo = 0, hi = lib->ntracks - 1, hit = -1;
	while (lo <= hi) {   /* первый с таким путём */
		int m = (lo + hi) / 2, c = strcmp(lib->tracks[by_path[m]].path, path);
		if (c < 0)
			lo = m + 1;
		else {
			if (!c)
				hit = m;
			hi = m - 1;
		}
	}
	if (hit < 0)
		return -1;
	int best = by_path[hit];
	for (int i = hit; i < lib->ntracks && !strcmp(lib->tracks[by_path[i]].path, path); i++)
		if (fabs(lib->tracks[by_path[i]].start - start) < 0.05)
			return by_path[i];
	return best;
}

/* «a/b/../c» → «a/c», «./» убирается */
static void normalize(char *p)
{
	for (char *c = p; *c; c++)
		if (*c == '\\')
			*c = '/';
	char *seg[128];
	int n = 0, abs = p[0] == '/';
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s", p);
	for (char *t = strtok(buf, "/"); t && n < 128; t = strtok(NULL, "/")) {
		if (!strcmp(t, "."))
			continue;
		if (!strcmp(t, "..")) {
			if (n)
				n--;
			continue;
		}
		seg[n++] = t;
	}
	char out[1024];
	int k = 0;
	out[0] = 0;
	for (int i = 0; i < n; i++)
		k += snprintf(out + k, sizeof(out) - k, "%s%s", (i || abs) ? "/" : "", seg[i]);
	snprintf(p, 1024, "%s", out);
}

static void load_one(playlist_t *p, const char *dir)
{
	FILE *f = fopen(p->file, "r");
	if (!f)
		return;
	char line[1100], path[1024];
	double start = 0;
	while (fgets(line, sizeof(line), f)) {
		char *s = line;
		if (!memcmp(s, "\xef\xbb\xbf", 3))
			s += 3;
		s[strcspn(s, "\r\n")] = 0;
		if (!strncmp(s, "#XMB-START:", 11)) {
			start = atof(s + 11);
			continue;
		}
		if (!s[0] || s[0] == '#')
			continue;
		if (s[0] == '/')
			snprintf(path, sizeof(path), "%s", s);
		else
			snprintf(path, sizeof(path), "%s/%s", dir, s);
		normalize(path);
		int t = find_track(path, start);
		if (t >= 0)
			pl_add(p, t);
		start = 0;
	}
	fclose(f);
}

static int ends_with(const char *s, const char *e)
{
	size_t a = strlen(s), b = strlen(e);
	return a >= b && !strcasecmp(s + a - b, e);
}

int pl_load_all(const library_t *L, const char *dir, playlist_t **out)
{
	lib = L;
	free(by_path);
	by_path = malloc(sizeof(int) * (L->ntracks ? L->ntracks : 1));
	for (int i = 0; i < L->ntracks; i++)
		by_path[i] = i;
	qsort(by_path, L->ntracks, sizeof(int), cmp_path);

	playlist_t *v = calloc(1, sizeof(playlist_t));
	int n = 1;
	char f[1100];
	snprintf(f, sizeof(f), "%s/%s.m3u8", dir, FAV_NAME);
	v[0].name = strdup(FAV_NAME);
	v[0].file = strdup(f);
	load_one(&v[0], dir);

	DIR *d = opendir(dir);
	struct dirent *e;
	while (d && (e = readdir(d))) {
		if (e->d_name[0] == '.' || !(ends_with(e->d_name, ".m3u8") || ends_with(e->d_name, ".m3u")))
			continue;
		snprintf(f, sizeof(f), "%s/%s", dir, e->d_name);
		if (!strcmp(f, v[0].file))
			continue;
		v = realloc(v, sizeof(playlist_t) * (n + 1));
		playlist_t *p = &v[n++];
		memset(p, 0, sizeof(*p));
		p->file = strdup(f);
		p->name = strdup(e->d_name);
		*strrchr(p->name, '.') = 0;
		load_one(p, dir);
	}
	if (d)
		closedir(d);
	*out = v;
	return n;
}

int pl_create(playlist_t **pls, int *n, const char *dir)
{
	char name[64], f[1100];
	struct stat st;
	for (int k = 1;; k++) {   /* первое свободное имя */
		snprintf(name, sizeof(name), "Плейлист %d", k);
		snprintf(f, sizeof(f), "%s/%s.m3u8", dir, name);
		int used = stat(f, &st) == 0;
		for (int i = 0; i < *n && !used; i++)
			used = !strcmp((*pls)[i].name, name);
		if (!used)
			break;
	}
	*pls = realloc(*pls, sizeof(playlist_t) * (*n + 1));
	playlist_t *p = &(*pls)[*n];
	memset(p, 0, sizeof(*p));
	p->name = strdup(name);
	p->file = strdup(f);
	return (*n)++;
}

int pl_find(const playlist_t *p, int track)
{
	for (int i = 0; i < p->n; i++)
		if (p->tracks[i] == track)
			return i;
	return -1;
}

void pl_add(playlist_t *p, int track)
{
	p->tracks = realloc(p->tracks, sizeof(int) * (p->n + 1));
	p->tracks[p->n++] = track;
}

void pl_remove(playlist_t *p, int pos)
{
	if (pos < 0 || pos >= p->n)
		return;
	memmove(p->tracks + pos, p->tracks + pos + 1, sizeof(int) * (p->n - pos - 1));
	p->n--;
}

/* путь трека относительно папки плейлиста: «../Исполнитель/Альбом/01.flac» */
static void rel_path(const char *dir, const char *path, char *out, size_t cap)
{
	size_t k = 0;
	while (dir[k] && dir[k] == path[k])
		k++;
	while (k && path[k - 1] != '/')   /* общая часть — до целой папки */
		k--;
	int up = 0;
	for (const char *c = dir + k; *c; c++)
		if (*c == '/' && c[1])
			up++;
	if (dir[k])
		up++;
	size_t o = 0;
	out[0] = 0;
	for (int i = 0; i < up && o + 3 < cap; i++)
		o += snprintf(out + o, cap - o, "../");
	snprintf(out + o, cap - o, "%s", path + k);
}

int pl_save(const library_t *L, const playlist_t *p, const char *root)
{
	char dir[1100];
	snprintf(dir, sizeof(dir), "%s", p->file);
	*strrchr(dir, '/') = 0;
	(void)root;
	mkdir(dir, 0755);
	char *buf = NULL;
	size_t len = 0;
	FILE *f = open_memstream(&buf, &len);
	if (!f)
		return -1;
	fprintf(f, "#EXTM3U\n");
	char rel[1100];
	for (int i = 0; i < p->n; i++) {
		const lib_track_t *t = &L->tracks[p->tracks[i]];
		fprintf(f, "#EXTINF:%d,%s - %s\n", (int)t->dur, t->artist, t->title);
		if (t->start > 0 || t->end > 0)
			fprintf(f, "#XMB-START:%.3f\n", t->start);
		rel_path(dir, t->path, rel, sizeof(rel));
		fprintf(f, "%s\n", rel);
	}
	fclose(f);
	fsave(p->file, buf, len);
	free(buf);
	return 0;
}

void pl_move(playlist_t *p, int pos, int dir)
{
	int q = pos + dir;
	if (pos < 0 || pos >= p->n || q < 0 || q >= p->n)
		return;
	int v = p->tracks[pos];
	p->tracks[pos] = p->tracks[q];
	p->tracks[q] = v;
}

int pl_rename(playlist_t *pls, int n, int i, const char *dir, const char *name)
{
	for (int k = 0; k < n; k++)
		if (k != i && !strcasecmp(pls[k].name, name))
			return -1;
	char f[1100];
	snprintf(f, sizeof(f), "%s/%s.m3u8", dir, name);
	struct stat st;
	if (strcmp(f, pls[i].file) && stat(f, &st) == 0)
		return -1;
	fsave_flush();   /* заказанная запись под старым именем иначе создала бы его заново */
	if (stat(pls[i].file, &st) == 0 && rename(pls[i].file, f) != 0)   /* ещё не сохранённый — файла нет */
		return -1;
	free(pls[i].name);
	free(pls[i].file);
	pls[i].name = strdup(name);
	pls[i].file = strdup(f);
	fsave_sync();
	return 0;
}

void pl_delete(playlist_t **pls, int *n, int i)
{
	playlist_t *v = *pls;
	fsave_flush();
	unlink(v[i].file);
	fsave_sync();
	free(v[i].name);
	free(v[i].file);
	free(v[i].tracks);
	memmove(v + i, v + i + 1, sizeof(playlist_t) * (*n - i - 1));
	(*n)--;
}
