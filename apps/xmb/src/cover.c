/*
 * Обложки альбомов → квадрат size×size RGB565. Декодирует фоновый поток, UI берёт готовое из кэша.
 * Baseline JPEG — TJpgDec с уменьшением 1/2…1/8 прямо при декодировании (любой размер файла);
 * PNG и прогрессивный JPEG — stb_image целиком, поэтому только до COVER_MAX_PX пикселей.
 * Источник — файл или «@смещение:длина:файл» (картинка из тегов). Каждая картинка декодируется
 * один раз: миниатюра THUMB×THUMB RGB888 сохраняется в кэш на карте, все размеры делаются из неё.
 */
#define _GNU_SOURCE   /* syscall(SYS_gettid) */
#include "cover.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "tjpgd.h"

#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#include "stb_image.h"
#pragma GCC diagnostic pop

#define SLOTS 256         /* записей в кэше: сетка альбомов (78 px) целиком переживает списки с обложками */
#define QUEUE 32
#define COVER_MAX_PX (2000 * 1000)
#define THUMB 256          /* не меньше самой крупной обложки на экране (плеер — 224): иначе она растягивается */

typedef struct {
	char *path;
	int size;
	int state;              /* 0 — пусто, 1 — в очереди, 2 — готово, 3 — ошибка */
	unsigned used;
	lv_image_dsc_t dsc;
	uint16_t *px;
} slot_t;

static slot_t slots[SLOTS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static int held, busy;   /* cover_hold: задания не берутся; busy — декодируется */
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned tick, generation;
static int queue[QUEUE], qn;
static char cache_dir[256];

/* ---------- источник: файл или кусок файла ---------- */

typedef struct {
	FILE *f;
	long start, len, pos;   /* len < 0 — до конца файла */
} src_t;

static int src_open(src_t *s, const char *spec)
{
	long off = 0, len = -1;
	const char *path = spec;
	if (spec[0] == '@') {
		char *e;
		off = strtol(spec + 1, &e, 10);
		len = *e == ':' ? strtol(e + 1, &e, 10) : -1;
		path = *e == ':' ? e + 1 : spec;
	}
	s->f = fopen(path, "rb");
	s->start = off;
	s->len = len;
	s->pos = 0;
	return s->f && fseek(s->f, off, SEEK_SET) == 0;
}

static size_t src_read(src_t *s, void *buf, size_t n)
{
	if (s->len >= 0 && s->pos + (long)n > s->len)
		n = s->pos < s->len ? (size_t)(s->len - s->pos) : 0;
	size_t r = buf ? fread(buf, 1, n, s->f) : (fseek(s->f, (long)n, SEEK_CUR) == 0 ? n : 0);
	s->pos += (long)r;
	return r;
}

static void src_rewind(src_t *s)
{
	fseek(s->f, s->start, SEEK_SET);
	s->pos = 0;
}

static int stb_read(void *u, char *buf, int n) { return (int)src_read(u, buf, (size_t)n); }
static void stb_skip(void *u, int n)
{
	src_t *s = u;
	if (n < 0) {
		fseek(s->f, n, SEEK_CUR);
		s->pos += n;
	} else {
		src_read(s, NULL, (size_t)n);
	}
}
static int stb_eof(void *u)
{
	src_t *s = u;
	return (s->len >= 0 && s->pos >= s->len) || feof(s->f);
}
static const stbi_io_callbacks stb_io = { stb_read, stb_skip, stb_eof };

/* ---------- декодеры: RGB888 w×h ---------- */

typedef struct {
	src_t *src;
	uint8_t *rgb;
	int w, h;
} job_t;

static size_t in_func(JDEC *jd, uint8_t *buf, size_t n)
{
	job_t *j = jd->device;
	return src_read(j->src, buf, n);
}

static int out_func(JDEC *jd, void *bitmap, JRECT *r)
{
	job_t *j = jd->device;
	uint8_t *src = bitmap;
	int w = r->right - r->left + 1;
	for (int y = r->top; y <= r->bottom && y < j->h; y++) {
		memcpy(j->rgb + (y * j->w + r->left) * 3, src, w * 3);
		src += w * 3;
	}
	return 1;
}

/* baseline JPEG: уменьшение при декодировании до ближайшего не меньше size */
static uint8_t *decode_tjpgd(src_t *s, int size, int *w, int *h)
{
	static uint8_t work[8192];
	JDEC jd;
	job_t j = { s, NULL, 0, 0 };
	if (jd_prepare(&jd, in_func, work, sizeof(work), &j) != JDR_OK)
		return NULL;
	int sc = 0, m = jd.width < jd.height ? jd.width : jd.height;
	while (sc < 3 && (m >> (sc + 1)) >= size)
		sc++;
	j.w = (jd.width + (1 << sc) - 1) >> sc;
	j.h = (jd.height + (1 << sc) - 1) >> sc;
	j.rgb = calloc(1, (size_t)j.w * j.h * 3);
	if (j.rgb && jd_decomp(&jd, out_func, (uint8_t)sc) != JDR_OK) {
		free(j.rgb);
		j.rgb = NULL;
	}
	*w = j.w;
	*h = j.h;
	return j.rgb;
}

static uint8_t *decode_stb(src_t *s, int *w, int *h)
{
	int c;
	if (!stbi_info_from_callbacks(&stb_io, s, w, h, &c) || (long)*w * *h > COVER_MAX_PX)
		return NULL;
	src_rewind(s);
	return stbi_load_from_callbacks(&stb_io, s, w, h, &c, 3);
}

/* пиксели источника под одним выходным: i0..i1, крайние — частично (веса в 1/256 пикселя), tot — сумма весов */
typedef struct {
	int i0, i1, w0, w1, tot;
} span_t;

static void spans(int side, int size, span_t *sp)
{
	for (int o = 0; o < size; o++) {
		long a = (long)o * side * 256 / size, b = (long)(o + 1) * side * 256 / size;
		sp[o].i0 = (int)(a >> 8);
		sp[o].i1 = (int)((b - 1) >> 8);
		sp[o].w0 = 256 - (int)(a & 255);
		sp[o].w1 = (int)((b - 1) & 255) + 1;
		if (sp[o].i0 == sp[o].i1)
			sp[o].w0 = sp[o].w1 = (int)(b - a);
		sp[o].tot = (int)(b - a);
	}
}

static int span_w(const span_t *s, int i)
{
	return i == s->i0 ? s->w0 : i == s->i1 ? s->w1 : 256;
}

/* квадрат по центру → size×size площадным усреднением с дробными весами (без ступенек при некратном
 * уменьшении): RGB888 (out565 = NULL) или RGB565. Сначала по строкам, потом по столбцам */
static void squash(const uint8_t *rgb, int w, int h, int size, uint8_t *out888, uint16_t *out565)
{
	int side = w < h ? w : h, x0 = (w - side) / 2, y0 = (h - side) / 2;
	span_t *sp = malloc(sizeof(span_t) * size);
	uint8_t *tmp = malloc((size_t)side * size * 3);
	if (!sp || !tmp) {
		free(sp);
		free(tmp);
		return;
	}
	spans(side, size, sp);
	for (int y = 0; y < side; y++) {
		const uint8_t *row = rgb + ((size_t)(y0 + y) * w + x0) * 3;
		for (int o = 0; o < size; o++) {
			unsigned r = 0, g = 0, b = 0;
			for (int i = sp[o].i0; i <= sp[o].i1; i++) {
				unsigned k = (unsigned)span_w(&sp[o], i);
				r += row[i * 3] * k;
				g += row[i * 3 + 1] * k;
				b += row[i * 3 + 2] * k;
			}
			uint8_t *d = tmp + ((size_t)y * size + o) * 3;
			d[0] = (uint8_t)(r / sp[o].tot);
			d[1] = (uint8_t)(g / sp[o].tot);
			d[2] = (uint8_t)(b / sp[o].tot);
		}
	}
	for (int o = 0; o < size; o++)
		for (int x = 0; x < size; x++) {
			unsigned r = 0, g = 0, b = 0;
			for (int i = sp[o].i0; i <= sp[o].i1; i++) {
				unsigned k = (unsigned)span_w(&sp[o], i);
				const uint8_t *s = tmp + ((size_t)i * size + x) * 3;
				r += s[0] * k;
				g += s[1] * k;
				b += s[2] * k;
			}
			r /= sp[o].tot;
			g /= sp[o].tot;
			b /= sp[o].tot;
			if (out565) {   /* упорядоченный дизеринг: без полос на плавных градиентах в 16 битах */
				static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
				unsigned dz = bayer[o & 3][x & 3];
				r = r + (dz >> 1) > 255 ? 255 : r + (dz >> 1);
				g = g + (dz >> 2) > 255 ? 255 : g + (dz >> 2);
				b = b + (dz >> 1) > 255 ? 255 : b + (dz >> 1);
				out565[o * size + x] = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
			} else {
				uint8_t *d = out888 + (o * size + x) * 3;
				d[0] = (uint8_t)r;
				d[1] = (uint8_t)g;
				d[2] = (uint8_t)b;
			}
		}
	free(tmp);
	free(sp);
}

/* имя миниатюры: хэш источника, размера и времени изменения файла */
static void thumb_name(const char *spec, char *out, size_t cap)
{
	const char *path = spec;
	if (spec[0] == '@') {
		const char *c = strchr(spec + 1, ':');
		c = c ? strchr(c + 1, ':') : NULL;
		path = c ? c + 1 : spec;
	}
	struct stat st;
	uint64_t h = 1469598103934665603ull;
	for (const char *p = spec; *p; p++)
		h = (h ^ (uint8_t)*p) * 1099511628211ull;
	if (stat(path, &st) == 0)
		h = (h ^ (uint64_t)st.st_size ^ ((uint64_t)st.st_mtime << 32)) * 1099511628211ull;
	snprintf(out, cap, "%s/%016llx_%d.rgb", cache_dir, (unsigned long long)h, THUMB);
}

static uint8_t *decode_thumb(const char *spec)
{
	src_t s;
	if (!src_open(&s, spec)) {
		if (s.f)
			fclose(s.f);
		return NULL;
	}
	int w = 0, h = 0, stb = 0;
	uint8_t *rgb = decode_tjpgd(&s, THUMB, &w, &h);
	if (!rgb) {   /* не baseline JPEG: PNG, прогрессивный */
		src_rewind(&s);
		rgb = decode_stb(&s, &w, &h);
		stb = 1;
	}
	fclose(s.f);
	uint8_t *th = rgb && w > 0 && h > 0 ? malloc(THUMB * THUMB * 3) : NULL;
	if (th)
		squash(rgb, w, h, THUMB, th, NULL);
	if (stb)
		stbi_image_free(rgb);
	else
		free(rgb);
	return th;
}

static uint16_t *decode(const char *spec, int size)
{
	char name[320];
	uint8_t *th = NULL;
	if (cache_dir[0]) {
		thumb_name(spec, name, sizeof(name));
		FILE *f = fopen(name, "rb");
		if (f) {
			th = malloc(THUMB * THUMB * 3);
			if (fread(th, 1, THUMB * THUMB * 3, f) != THUMB * THUMB * 3) {
				free(th);
				th = NULL;
			}
			fclose(f);
		}
	}
	if (!th) {
		th = decode_thumb(spec);
		if (th && cache_dir[0]) {
			FILE *f = fopen(name, "wb");
			if (f) {
				fwrite(th, 1, THUMB * THUMB * 3, f);
				fclose(f);
			}
		}
	}
	uint16_t *out = th ? malloc((size_t)size * size * 2) : NULL;
	if (out)
		squash(th, THUMB, THUMB, size, NULL, out);
	free(th);
	return out;
}

/* ---------- поток и кэш ---------- */

static void *worker(void *arg)
{
	(void)arg;
	/* ядро одно: декодирование обложек не должно отнимать время у кадров интерфейса. nice 10 не хватало —
	 * поток всё равно получал свою долю посреди кадра; SCHED_IDLE — только когда остальным процессор не нужен
	 * (между кадрами его полно) */
	struct sched_param sp = { 0 };
	if (pthread_setschedparam(pthread_self(), SCHED_IDLE, &sp) != 0 &&
	    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19) < 0)
		perror("cover: setpriority");
	for (;;) {
		pthread_mutex_lock(&mu);
		while (!qn || held)
			pthread_cond_wait(&cv, &mu);
		busy = 1;
		int i = queue[--qn];   /* свежие запросы — первыми: это то, что сейчас на экране */
		char *path = strdup(slots[i].path);
		int size = slots[i].size;
		pthread_mutex_unlock(&mu);

		uint16_t *px = decode(path, size);

		pthread_mutex_lock(&mu);
		if (slots[i].path && !strcmp(slots[i].path, path) && slots[i].size == size && slots[i].state == 1) {
			slots[i].px = px;
			slots[i].state = px ? 2 : 3;
			if (px) {
				lv_image_dsc_t *d = &slots[i].dsc;
				memset(d, 0, sizeof(*d));
				d->header.magic = LV_IMAGE_HEADER_MAGIC;
				d->header.cf = LV_COLOR_FORMAT_RGB565;
				d->header.w = size;
				d->header.h = size;
				d->header.stride = size * 2;
				d->data_size = size * size * 2;
				d->data = (const uint8_t *)px;
			}
			generation++;
		} else {
			free(px);
		}
		busy = 0;
		pthread_cond_broadcast(&cv);
		pthread_mutex_unlock(&mu);
		free(path);
	}
	return NULL;
}

void cover_init(const char *dir)
{
	if (dir) {
		snprintf(cache_dir, sizeof(cache_dir), "%s", dir);
		mkdir(cache_dir, 0755);
	}
	/* stb_image держит на стеке заметно больше, чем 128 КБ потока musl по умолчанию */
	pthread_attr_t a;
	pthread_attr_init(&a);
	pthread_attr_setstacksize(&a, 512 * 1024);
	pthread_t t;
	pthread_create(&t, &a, worker, NULL);
	pthread_attr_destroy(&a);
}

unsigned cover_generation(void)
{
	return generation;
}

void cover_hold(int hold)
{
	pthread_mutex_lock(&mu);
	held = hold;
	while (hold && busy)
		pthread_cond_wait(&cv, &mu);
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
}

int cover_ready(const char *path, int size)
{
	int r = -1;
	pthread_mutex_lock(&mu);
	for (int i = 0; i < SLOTS; i++)
		if (slots[i].path && slots[i].size == size && !strcmp(slots[i].path, path)) {
			r = slots[i].state == 2 ? 1 : slots[i].state == 1 ? 0 : -1;
			break;
		}
	pthread_mutex_unlock(&mu);
	return r;
}

const lv_image_dsc_t *cover_get(const char *path, int size)
{
	if (!path)
		return NULL;
	pthread_mutex_lock(&mu);
	tick++;
	int free_i = -1, old = 0;
	for (int i = 0; i < SLOTS; i++) {
		if (slots[i].path && slots[i].size == size && !strcmp(slots[i].path, path)) {
			slots[i].used = tick;
			const lv_image_dsc_t *d = slots[i].state == 2 ? &slots[i].dsc : NULL;
			pthread_mutex_unlock(&mu);
			return d;
		}
		if (!slots[i].path && free_i < 0)
			free_i = i;
		if (slots[i].state != 1 && slots[i].used < slots[old].used)
			old = i;
	}
	/* новая запись: свободная или давно не нужная (не та, что сейчас декодируется) */
	int i = free_i >= 0 ? free_i : old;
	if (slots[i].state == 1) {
		pthread_mutex_unlock(&mu);
		return NULL;
	}
	free(slots[i].path);
	free(slots[i].px);
	slots[i] = (slot_t){ strdup(path), size, 1, tick, { { 0 } }, NULL };
	if (qn == QUEUE) {   /* очередь полна — самый старый запрос отменяется */
		slots[queue[0]].state = 0;
		free(slots[queue[0]].path);
		slots[queue[0]].path = NULL;
		memmove(queue, queue + 1, sizeof(int) * (QUEUE - 1));
		qn--;
	}
	queue[qn++] = i;
	pthread_cond_signal(&cv);
	pthread_mutex_unlock(&mu);
	return NULL;
}
