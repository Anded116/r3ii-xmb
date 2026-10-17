/*
 * Музыкальная библиотека.
 *
 * Теги: FLAC — STREAMINFO и VORBIS_COMMENT; M4A (ALAC и AAC) — атомы iTunes через mp4.c из Sonix; MP3 — ID3v2.2/2.3/2.4 (ISO-8859-1 в старых русских
 * файлах на деле cp1251 — распознаётся), длительность по Xing/Info/VBRI или по битрейту CBR; WAV — fmt/data.
 * Без тегов: название — имя файла без номера, альбом — папка.
 * Альбомы группируются по тегу альбома + исполнителю альбома; папки «Disc N»/«CD N» — часть альбома выше.
 * Кэш тегов — текстовый файл: неизменившиеся (размер + время) файлы не перечитываются.
 * CUE: образ альбома одним файлом + лист .cue — треки внутри файла (start/end), сам образ треком не считается.
 * Обложка альбома — картинка из папки (front/cover/folder, без задников и вкладышей) или из тегов
 * (FLAC PICTURE, ID3 APIC): тогда это строка «@смещение:длина:путь».
 */
#include "library.h"
#include "mp4.h"

#include <ctype.h>
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_TAG 512
#define CACHE_MAGIC "#xmb-library 3"

/* ---------- строки ---------- */

static char *xstrdup(const char *s)
{
	return strdup(s ? s : "");
}

static void put_utf8(char **o, char *end, unsigned cp)
{
	char *p = *o;
	if (cp < 0x80 && p + 1 < end) *p++ = (char)cp;
	else if (cp < 0x800 && p + 2 < end) { *p++ = (char)(0xc0 | cp >> 6); *p++ = (char)(0x80 | (cp & 63)); }
	else if (cp < 0x10000 && p + 3 < end) { *p++ = (char)(0xe0 | cp >> 12); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
	else if (p + 4 < end) { *p++ = (char)(0xf0 | cp >> 18); *p++ = (char)(0x80 | ((cp >> 12) & 63)); *p++ = (char)(0x80 | ((cp >> 6) & 63)); *p++ = (char)(0x80 | (cp & 63)); }
	*o = p;
}

static unsigned cp1251(unsigned char c)
{
	static const unsigned short hi[64] = {   /* 0x80..0xbf */
		0x402, 0x403, 0x201a, 0x453, 0x201e, 0x2026, 0x2020, 0x2021, 0x20ac, 0x2030, 0x409, 0x2039, 0x40a, 0x40c, 0x40b, 0x40f,
		0x452, 0x2018, 0x2019, 0x201c, 0x201d, 0x2022, 0x2013, 0x2014, 0x20, 0x2122, 0x459, 0x203a, 0x45a, 0x45c, 0x45b, 0x45f,
		0xa0, 0x40e, 0x45e, 0x408, 0xa4, 0x490, 0xa6, 0xa7, 0x401, 0xa9, 0x404, 0xab, 0xac, 0xad, 0xae, 0x407,
		0xb0, 0xb1, 0x406, 0x456, 0x491, 0xb5, 0xb6, 0xb7, 0x451, 0x2116, 0x454, 0xbb, 0x458, 0x405, 0x455, 0x457,
	};
	return c < 0x80 ? c : c < 0xc0 ? hi[c - 0x80] : 0x410 + (c - 0xc0);
}

static int valid_utf8(const unsigned char *s, int n)
{
	for (int i = 0; i < n && s[i]; i++) {
		if (s[i] < 0x80)
			continue;
		int k = (s[i] & 0xe0) == 0xc0 ? 1 : (s[i] & 0xf0) == 0xe0 ? 2 : (s[i] & 0xf8) == 0xf0 ? 3 : -1;
		if (k < 0)
			return 0;
		for (int j = 1; j <= k; j++)
			if (i + j >= n || (s[i + j] & 0xc0) != 0x80)
				return 0;
		i += k;
	}
	return 1;
}

/* однобайтовый текст: валидный UTF-8 оставляем, иначе считаем cp1251 (так пишут старые русские теги) */
static void bytes_to_utf8(const unsigned char *s, int n, char *out, int cap)
{
	int utf8 = valid_utf8(s, n);
	char *o = out, *end = out + cap;
	for (int i = 0; i < n && s[i]; i++) {
		if (utf8 || s[i] < 0x80) {
			if (o + 1 < end)
				*o++ = (char)s[i];
		} else {
			put_utf8(&o, end, cp1251(s[i]));
		}
	}
	*o = 0;
}

static void utf16_to_utf8(const unsigned char *s, int n, int be, char *out, int cap)
{
	char *o = out, *end = out + cap;
	if (n >= 2 && ((s[0] == 0xff && s[1] == 0xfe) || (s[0] == 0xfe && s[1] == 0xff))) {
		be = s[0] == 0xfe;
		s += 2;
		n -= 2;
	}
	for (int i = 0; i + 1 < n; i += 2) {
		unsigned c = be ? (s[i] << 8 | s[i + 1]) : (s[i + 1] << 8 | s[i]);
		if (!c)
			break;
		if (c >= 0xd800 && c < 0xdc00 && i + 3 < n) {
			unsigned c2 = be ? (s[i + 2] << 8 | s[i + 3]) : (s[i + 3] << 8 | s[i + 2]);
			c = 0x10000 + ((c - 0xd800) << 10) + (c2 - 0xdc00);
			i += 2;
		}
		put_utf8(&o, end, c);
	}
	*o = 0;
}

static void trim(char *s)
{
	int n = (int)strlen(s);
	while (n && (unsigned char)s[n - 1] <= ' ')
		s[--n] = 0;
	int k = 0;
	while (s[k] && (unsigned char)s[k] <= ' ')
		k++;
	if (k)
		memmove(s, s + k, n - k + 1);
}

/* следующая буква строки в верхнем регистре (латиница, кириллица; Ё → Е) */
static unsigned fold_next(const unsigned char **p)
{
	const unsigned char *s = *p;
	unsigned cp = *s++;
	if ((cp & 0xe0) == 0xc0 && *s) {
		cp = (cp & 31) << 6 | (*s++ & 63);
	} else if ((cp & 0xf0) == 0xe0 && s[0] && s[1]) {
		cp = (cp & 15) << 12 | (s[0] & 63) << 6 | (s[1] & 63);
		s += 2;
	}
	*p = s;
	if (cp >= 'a' && cp <= 'z')
		return cp - 32;
	if (cp >= 0x430 && cp <= 0x44f)
		return cp - 0x20;
	if (cp == 0x451 || cp == 0x401)
		return 0x415;
	return cp;
}

unsigned lib_fold(const char **p)
{
	return fold_next((const unsigned char **)p);
}

int lib_strcmp(const char *a, const char *b)
{
	const unsigned char *p = (const unsigned char *)a, *q = (const unsigned char *)b;
	/* ведущие кавычки, скобки, пробелы не считаются — как у буквы указателя */
	while (*p && *p < 0x80 && !((*p >= '0' && *p <= '9') || ((*p | 32) >= 'a' && (*p | 32) <= 'z')))
		p++;
	while (*q && *q < 0x80 && !((*q >= '0' && *q <= '9') || ((*q | 32) >= 'a' && (*q | 32) <= 'z')))
		q++;
	while (*p && *q) {
		unsigned x = fold_next(&p), y = fold_next(&q);
		if (x != y)
			return x < y ? -1 : 1;
	}
	return *p ? 1 : *q ? -1 : 0;
}

/* ---------- теги ---------- */

typedef struct {
	char title[MAX_TAG], artist[MAX_TAG], album[MAX_TAG], albumartist[MAX_TAG];
	int track, disc, year;
	double dur;
	long pic_off;       /* картинка в тегах: смещение и длина данных в файле */
	long pic_len;
	int pic_front;
	float rg_track, rg_album, rg_tpeak, rg_apeak;   /* ReplayGain, LIB_RG_NONE — нет */
} tags_t;

static void tags_init(tags_t *t)
{
	memset(t, 0, sizeof(*t));
	t->rg_track = t->rg_album = t->rg_tpeak = t->rg_apeak = LIB_RG_NONE;
}

/* картинка из тегов: обложка (тип 3) важнее любой другой */
static void set_pic(tags_t *t, long off, long len, int type)
{
	int front = type == 3;
	if (len > 0 && (!t->pic_len || (front && !t->pic_front))) {
		t->pic_off = off;
		t->pic_len = len;
		t->pic_front = front;
	}
}

static void set_tag(tags_t *t, const char *key, const char *val)
{
	char *dst = NULL;
	if (!strcasecmp(key, "TITLE") || !strcmp(key, "TIT2") || !strcmp(key, "TT2")) dst = t->title;
	else if (!strcasecmp(key, "ARTIST") || !strcmp(key, "TPE1") || !strcmp(key, "TP1")) dst = t->artist;
	else if (!strcasecmp(key, "ALBUM") || !strcmp(key, "TALB") || !strcmp(key, "TAL")) dst = t->album;
	else if (!strcasecmp(key, "ALBUMARTIST") || !strcasecmp(key, "ALBUM ARTIST") || !strcmp(key, "TPE2") || !strcmp(key, "TP2")) dst = t->albumartist;
	else if (!strcasecmp(key, "TRACKNUMBER") || !strcmp(key, "TRCK") || !strcmp(key, "TRK")) t->track = atoi(val);
	else if (!strcasecmp(key, "DISCNUMBER") || !strcmp(key, "TPOS") || !strcmp(key, "TPA")) t->disc = atoi(val);
	else if (!strcasecmp(key, "DATE") || !strcasecmp(key, "YEAR") || !strcmp(key, "TYER") || !strcmp(key, "TDRC") || !strcmp(key, "TYE")) t->year = atoi(val);
	else if (!strcasecmp(key, "REPLAYGAIN_TRACK_GAIN")) t->rg_track = (float)atof(val);   /* «-6.54 dB» */
	else if (!strcasecmp(key, "REPLAYGAIN_ALBUM_GAIN")) t->rg_album = (float)atof(val);
	else if (!strcasecmp(key, "REPLAYGAIN_TRACK_PEAK")) t->rg_tpeak = (float)atof(val);
	else if (!strcasecmp(key, "REPLAYGAIN_ALBUM_PEAK")) t->rg_apeak = (float)atof(val);
	if (dst && !dst[0]) {
		snprintf(dst, MAX_TAG, "%s", val);
		trim(dst);
	}
}

static uint32_t be32(const unsigned char *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const unsigned char *p) { return (uint32_t)p[3] << 24 | p[2] << 16 | p[1] << 8 | p[0]; }
static uint32_t syncsafe(const unsigned char *p) { return (uint32_t)(p[0] & 127) << 21 | (p[1] & 127) << 14 | (p[2] & 127) << 7 | (p[3] & 127); }

static void parse_flac(FILE *f, tags_t *t)
{
	unsigned char h[4];
	if (fread(h, 1, 4, f) != 4 || memcmp(h, "fLaC", 4))
		return;
	for (int last = 0; !last;) {
		if (fread(h, 1, 4, f) != 4)
			return;
		last = h[0] & 0x80;
		int type = h[0] & 127;
		uint32_t len = (uint32_t)h[1] << 16 | h[2] << 8 | h[3];
		if (type == 0 && len >= 18) {
			unsigned char si[34];
			if (fread(si, 1, 34, f) != 34)
				return;
			uint32_t rate = (uint32_t)si[10] << 12 | si[11] << 4 | si[12] >> 4;
			uint64_t total = ((uint64_t)(si[13] & 15) << 32) | be32(si + 14);
			if (rate)
				t->dur = (double)total / rate;
			fseek(f, (long)len - 34, SEEK_CUR);
		} else if (type == 4 && len < (1u << 20)) {
			unsigned char *b = malloc(len + 1);
			if (!b || fread(b, 1, len, f) != len) {
				free(b);
				return;
			}
			uint32_t p = 4 + le32(b), n = p + 4 <= len ? le32(b + p) : 0;
			p += 4;
			for (uint32_t i = 0; i < n && p + 4 <= len; i++) {
				uint32_t l = le32(b + p);
				p += 4;
				if (p + l > len)
					break;
				char kv[MAX_TAG * 2];
				int m = l < sizeof(kv) - 1 ? (int)l : (int)sizeof(kv) - 1;
				memcpy(kv, b + p, m);
				kv[m] = 0;
				char *eq = strchr(kv, '=');
				if (eq) {
					*eq = 0;
					set_tag(t, kv, eq + 1);
				}
				p += l;
			}
			free(b);
		} else if (type == 6 && len > 32) {
			/* PICTURE: тип, MIME, описание, размеры, длина данных, данные */
			long start = ftell(f);
			unsigned char b[8];
			uint32_t ptype = 0, l = 0;
			if (fread(b, 1, 8, f) == 8) {
				ptype = be32(b);
				l = be32(b + 4);
				fseek(f, l, SEEK_CUR);
				if (fread(b, 1, 4, f) == 4) {
					fseek(f, be32(b) + 16, SEEK_CUR);
					if (fread(b, 1, 4, f) == 4 && ftell(f) + (long)be32(b) <= start + (long)len)
						set_pic(t, ftell(f), be32(b), (int)ptype);
				}
			}
			fseek(f, start + len, SEEK_SET);
		} else {
			fseek(f, len, SEEK_CUR);
		}
	}
}

static void id3_text(tags_t *t, const char *id, const unsigned char *d, int n)
{
	if (n < 1)
		return;
	char out[MAX_TAG];
	int enc = d[0];
	if (enc == 1 || enc == 2)
		utf16_to_utf8(d + 1, n - 1, enc == 2, out, sizeof(out));
	else
		bytes_to_utf8(d + 1, n - 1, out, sizeof(out));
	set_tag(t, id, out);
}

/* TXXX: кодировка, описание до нуля, значение — ReplayGain в MP3 пишут так */
static void id3_txxx(tags_t *t, const unsigned char *d, int n)
{
	if (n < 2)
		return;
	char desc[MAX_TAG], val[MAX_TAG];
	int enc = d[0], wide = enc == 1 || enc == 2, i = 1;
	while (i < n && (wide ? (i + 1 < n && (d[i] || d[i + 1])) : d[i] != 0))
		i += wide ? 2 : 1;
	if (wide) {
		utf16_to_utf8(d + 1, i - 1, enc == 2, desc, sizeof(desc));
		utf16_to_utf8(d + i + 2, n - i - 2, enc == 2, val, sizeof(val));
	} else {
		bytes_to_utf8(d + 1, i - 1, desc, sizeof(desc));
		bytes_to_utf8(d + i + 1, n - i - 1, val, sizeof(val));
	}
	set_tag(t, desc, val);
}

static long parse_id3(FILE *f, tags_t *t)
{
	unsigned char h[10];
	if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3))
		return 0;
	int ver = h[3], unsync = h[5] & 0x80;
	long size = syncsafe(h + 6), end = 10 + size;
	if (h[5] & 0x40) {   /* расширенный заголовок */
		unsigned char e[4];
		if (fread(e, 1, 4, f) != 4)
			return end;
		fseek(f, (ver == 4 ? syncsafe(e) : be32(e)) - (ver == 4 ? 4 : 0), SEEK_CUR);
	}
	while (ftell(f) + (ver == 2 ? 6 : 10) < end) {
		unsigned char fh[10];
		int hl = ver == 2 ? 6 : 10;
		if (fread(fh, 1, hl, f) != (size_t)hl || !fh[0])
			break;
		char id[5] = { 0 };
		memcpy(id, fh, ver == 2 ? 3 : 4);
		uint32_t len = ver == 2 ? ((uint32_t)fh[3] << 16 | fh[4] << 8 | fh[5]) : ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
		if (len > (uint32_t)size)
			break;
		if (id[0] == 'T' && len < 4096) {
			unsigned char d[4096];
			if (fread(d, 1, len, f) != len)
				break;
			if (!strcmp(id, "TXXX") || !strcmp(id, "TXX"))
				id3_txxx(t, d, (int)len);
			else
				id3_text(t, id, d, (int)len);
		} else if ((!strcmp(id, "APIC") || !strcmp(id, "PIC")) && !unsync && !(ver == 4 && (fh[9] & 0x03)) && len > 16) {
			/* APIC: кодировка, MIME до нуля, тип, описание до нуля (UTF-16 — до двух нулей), данные;
			 * PIC (v2.2): кодировка, формат из 3 букв, тип, описание */
			long start = ftell(f);
			unsigned char d[512];
			int n = (int)fread(d, 1, len < sizeof(d) ? len : sizeof(d), f), i = 1;
			if (ver == 2)
				i += 3;
			else
				while (i < n && d[i])
					i++;
			if (ver != 2)
				i++;
			int ptype = i < n ? d[i] : 0, wide = d[0] == 1 || d[0] == 2;
			i++;
			while (i < n && (wide ? (i + 1 < n && (d[i] || d[i + 1])) : d[i] != 0))
				i += wide ? 2 : 1;
			i += wide ? 2 : 1;
			if (i < n)
				set_pic(t, start + i, (long)len - i, ptype);
			fseek(f, start + len, SEEK_SET);
		} else {
			fseek(f, len, SEEK_CUR);
		}
	}
	return end;
}

static void mp3_duration(FILE *f, long start, long fsize, tags_t *t)
{
	static const int br[2][16] = {
		{ 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 },   /* MPEG1 Layer III */
		{ 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },        /* MPEG2/2.5 Layer III */
	};
	static const int sr[4][3] = { { 11025, 12000, 8000 }, { 0 }, { 22050, 24000, 16000 }, { 44100, 48000, 32000 } };
	unsigned char b[4096];
	fseek(f, start, SEEK_SET);
	int n = (int)fread(b, 1, sizeof(b), f);
	for (int i = 0; i + 4 < n; i++) {
		if (b[i] != 0xff || (b[i + 1] & 0xe0) != 0xe0)
			continue;
		int v = (b[i + 1] >> 3) & 3, layer = (b[i + 1] >> 1) & 3, bi = b[i + 2] >> 4, si = (b[i + 2] >> 2) & 3;
		if (v == 1 || layer != 1 || bi == 0 || bi == 15 || si == 3)
			continue;
		int mono = (b[i + 3] >> 6) == 3, rate = sr[v][si], kbps = br[v == 3 ? 0 : 1][bi];
		int spf = v == 3 ? 1152 : 576;
		int side = v == 3 ? (mono ? 17 : 32) : (mono ? 9 : 17);
		int x = i + 4 + side;
		if (x + 12 < n && (!memcmp(b + x, "Xing", 4) || !memcmp(b + x, "Info", 4)) && (b[x + 7] & 1)) {
			t->dur = (double)be32(b + x + 8) * spf / rate;
			return;
		}
		if (i + 36 + 18 < n && !memcmp(b + i + 36, "VBRI", 4)) {
			t->dur = (double)be32(b + i + 36 + 14) * spf / rate;
			return;
		}
		t->dur = (double)(fsize - start - i) * 8 / (kbps * 1000.0);
		return;
	}
}

static void parse_wav(FILE *f, tags_t *t)
{
	unsigned char h[12], c[8];
	if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
		return;
	uint32_t byterate = 0;
	while (fread(c, 1, 8, f) == 8) {
		uint32_t len = le32(c + 4);
		if (!memcmp(c, "fmt ", 4) && len >= 16) {
			unsigned char fm[16];
			if (fread(fm, 1, 16, f) != 16)
				return;
			byterate = le32(fm + 8);
			fseek(f, (long)len - 16 + (len & 1), SEEK_CUR);
		} else if (!memcmp(c, "data", 4)) {
			if (byterate)
				t->dur = (double)len / byterate;
			return;
		} else {
			fseek(f, (long)len + (len & 1), SEEK_CUR);
		}
	}
}

/* M4A: теги iTunes и обложка (covr); не ALAC и не AAC (видео, другой кодек) — dur = −1, в библиотеку не попадает */
static void read_m4a(const char *path, tags_t *t)
{
	mp4_file_t *m = mp4_open(path);
	if (!m || (mp4_audio_codec(m) != MP4_CODEC_ALAC && mp4_audio_codec(m) != MP4_CODEC_AAC) || mp4_has_video(m)) {
		t->dur = -1;
		if (m)
			mp4_close(m);
		return;
	}
	snprintf(t->title, MAX_TAG, "%s", mp4_tag_title(m));
	snprintf(t->artist, MAX_TAG, "%s", mp4_tag_artist(m));
	snprintf(t->album, MAX_TAG, "%s", mp4_tag_album(m));
	snprintf(t->albumartist, MAX_TAG, "%s", mp4_tag_album_artist(m));
	t->track = mp4_tag_track_number(m);
	t->disc = mp4_tag_disc_number(m);
	t->year = mp4_tag_year(m);
	t->dur = mp4_duration_seconds(m);
	static const char *const rg[4] = { "replaygain_track_gain", "replaygain_album_gain", "replaygain_track_peak",
					   "replaygain_album_peak" };
	for (int i = 0; i < 4; i++) {
		const char *v = mp4_tag_freeform(m, rg[i]);
		if (v)
			set_tag(t, rg[i], v);
	}
	uint64_t off;
	uint32_t size;
	bool png;
	if (mp4_cover_art(m, &off, &size, &png))
		set_pic(t, (long)off, (long)size, 3);
	mp4_close(m);
}

static void read_tags(const char *path, const char *ext, tags_t *t)
{
	if (!strcasecmp(ext, "m4a")) {
		read_m4a(path, t);
		return;
	}
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	fseek(f, 0, SEEK_END);
	long size = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (!strcasecmp(ext, "flac")) {
		parse_flac(f, t);
	} else if (!strcasecmp(ext, "mp3")) {
		long start = parse_id3(f, t);
		mp3_duration(f, start, size, t);
	} else if (!strcasecmp(ext, "wav")) {
		parse_wav(f, t);
	}
	fclose(f);
}

/* ---------- кэш ---------- */

typedef struct {
	char *path;
	long size, mtime;
	tags_t tags;
} cache_t;

static cache_t *cache;
static int ncache, capcache;
static cache_t *fresh;
static int nfresh, capfresh;

/* картинки-кандидаты в обложки: годится ли и сколько пикселей — чтобы не читать заголовок при каждом скане */
typedef struct {
	char *path;
	long size, mtime, px;
	int usable;
} img_t;

static img_t *imgs, *fresh_imgs;
static int nimgs, capimgs, nfresh_imgs, capfresh_imgs;

/* картинки, которые встретил обход: обложки ищутся по этому списку, папки второй раз не читаются */
static char **seen;
static int nseen, capseen;

static int is_image(const char *name)
{
	const char *dot = strrchr(name, '.');
	return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg") || !strcasecmp(dot, ".png"));
}

static int img_cmp(const void *a, const void *b)
{
	return strcmp(((const img_t *)a)->path, ((const img_t *)b)->path);
}

static void img_push(img_t **arr, int *n, int *cap, img_t v)
{
	if (*n == *cap) {
		*cap = *cap ? *cap * 2 : 256;
		*arr = realloc(*arr, *cap * sizeof(**arr));
	}
	(*arr)[(*n)++] = v;
}

static int cache_cmp(const void *a, const void *b)
{
	return strcmp(((const cache_t *)a)->path, ((const cache_t *)b)->path);
}

static void cache_load(const char *file)
{
	FILE *f = fopen(file, "r");
	if (!f)
		return;
	static char line[8192];
	if (!fgets(line, sizeof(line), f) || strncmp(line, CACHE_MAGIC, strlen(CACHE_MAGIC))) {
		fclose(f);   /* старый формат — всё перечитывается один раз */
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\n")] = 0;
		if (!strncmp(line, "@img\t", 5)) {   /* @img путь размер время годится пикселей */
			char *q = line + 5, *tab = strchr(q, '\t');
			long size, mtime, px;
			int usable;
			if (tab && sscanf(tab + 1, "%ld\t%ld\t%d\t%ld", &size, &mtime, &usable, &px) == 4) {
				*tab = 0;
				img_push(&imgs, &nimgs, &capimgs, (img_t){ strdup(q), size, mtime, px, usable });
			}
			continue;
		}
		char *col[17];
		int n = 0;
		for (char *p = line; n < 17; n++) {
			col[n] = p;
			char *tab = strchr(p, '\t');
			if (!tab) { n++; break; }
			*tab = 0;
			p = tab + 1;
		}
		if (n < 17)
			continue;
		if (ncache == capcache) {
			capcache = capcache ? capcache * 2 : 1024;
			cache = realloc(cache, capcache * sizeof(*cache));
		}
		cache_t *c = &cache[ncache++];
		memset(c, 0, sizeof(*c));
		c->path = strdup(col[0]);
		c->size = atol(col[1]);
		c->mtime = atol(col[2]);
		c->tags.dur = atof(col[3]);
		c->tags.track = atoi(col[4]);
		c->tags.disc = atoi(col[5]);
		c->tags.year = atoi(col[6]);
		snprintf(c->tags.title, MAX_TAG, "%s", col[7]);
		snprintf(c->tags.artist, MAX_TAG, "%s", col[8]);
		snprintf(c->tags.album, MAX_TAG, "%s", col[9]);
		snprintf(c->tags.albumartist, MAX_TAG, "%s", col[10]);
		c->tags.pic_off = atol(col[11]);
		c->tags.pic_len = atol(col[12]);
		c->tags.rg_track = (float)atof(col[13]);
		c->tags.rg_album = (float)atof(col[14]);
		c->tags.rg_tpeak = (float)atof(col[15]);
		c->tags.rg_apeak = (float)atof(col[16]);
	}
	fclose(f);
	qsort(cache, ncache, sizeof(*cache), cache_cmp);
	qsort(imgs, nimgs, sizeof(*imgs), img_cmp);
}

static void cache_save(const char *file)
{
	FILE *f = fopen(file, "w");
	if (!f)
		return;
	fprintf(f, "%s\n", CACHE_MAGIC);
	for (int i = 0; i < nfresh; i++) {
		cache_t *c = &fresh[i];
		fprintf(f, "%s\t%ld\t%ld\t%.3f\t%d\t%d\t%d\t%s\t%s\t%s\t%s\t%ld\t%ld\t%.2f\t%.2f\t%.6f\t%.6f\n", c->path,
			c->size, c->mtime, c->tags.dur, c->tags.track, c->tags.disc, c->tags.year, c->tags.title, c->tags.artist,
			c->tags.album, c->tags.albumartist, c->tags.pic_off, c->tags.pic_len, c->tags.rg_track, c->tags.rg_album,
			c->tags.rg_tpeak, c->tags.rg_apeak);
	}
	for (int i = 0; i < nfresh_imgs; i++) {
		img_t *m = &fresh_imgs[i];
		fprintf(f, "@img\t%s\t%ld\t%ld\t%d\t%ld\n", m->path, m->size, m->mtime, m->usable, m->px);
	}
	fclose(f);
}

/* ---------- обход ---------- */

static library_t *L;
static int captracks;
static volatile int *prog;

static int is_podcast(const char *path);

static int is_audio(const char *name, const char **ext)
{
	const char *e = strrchr(name, '.');
	if (!e)
		return 0;
	*ext = e + 1;
	return !strcasecmp(e, ".flac") || !strcasecmp(e, ".mp3") || !strcasecmp(e, ".wav") || !strcasecmp(e, ".m4a");
}

static int skip_dir(const char *name)
{
	return name[0] == '.' || !strcmp(name, "System Volume Information") || !strcmp(name, "hiby_dev") ||
	       !strcmp(name, "screensavers");
}

/* название из имени файла: без расширения и ведущего номера «01 - », «01. » */
static void title_from_name(const char *name, char *out)
{
	const char *p = name;
	while (isdigit((unsigned char)*p))
		p++;
	if (p != name && (*p == '.' || *p == '-' || *p == ' ' || *p == '_')) {
		while (*p == '.' || *p == '-' || *p == ' ' || *p == '_')
			p++;
	} else {
		p = name;
	}
	snprintf(out, MAX_TAG, "%s", p);
	char *dot = strrchr(out, '.');
	if (dot)
		*dot = 0;
}

static int name_cmp(const void *a, const void *b)   /* имена в walk — с байтом типа впереди */
{
	return lib_strcmp(*(char *const *)a + 1, *(char *const *)b + 1);
}

/* теги файла: из кэша, если файл не менялся, иначе чтением; попадают в новый кэш */
static void get_tags(const char *path, const char *ext, struct stat *st, tags_t *t)
{
	cache_t key = { .path = (char *)path }, *c = ncache ? bsearch(&key, cache, ncache, sizeof(*cache), cache_cmp) : NULL;
	if (c && c->size == (long)st->st_size && c->mtime == (long)st->st_mtime) {
		*t = c->tags;
	} else {
		tags_init(t);
		read_tags(path, ext, t);
	}
	if (nfresh == capfresh) {
		capfresh = capfresh ? capfresh * 2 : 1024;
		fresh = realloc(fresh, capfresh * sizeof(*fresh));
	}
	fresh[nfresh++] = (cache_t){ strdup(path), (long)st->st_size, (long)st->st_mtime, *t };
}

static lib_track_t *new_track(lib_dir_t *dir)
{
	if (L->ntracks == captracks) {
		captracks = captracks ? captracks * 2 : 1024;
		L->tracks = realloc(L->tracks, captracks * sizeof(*L->tracks));
	}
	lib_track_t *tr = &L->tracks[L->ntracks];
	memset(tr, 0, sizeof(*tr));
	tr->aidx = -1;
	dir->tracks = realloc(dir->tracks, (dir->ntracks + 1) * sizeof(int));
	dir->tracks[dir->ntracks++] = L->ntracks++;
	if (prog)
		(*prog)++;
	return tr;
}

static void drop_url(char *s);

static void add_track(const char *path, const char *name, const char *ext, struct stat *st, lib_dir_t *dir)
{
	tags_t t;
	get_tags(path, ext, st, &t);
	if (t.dur < 0)   /* формат не поддержан (AAC) */
		return;
	drop_url(t.artist);
	drop_url(t.albumartist);
	lib_track_t *tr = new_track(dir);
	char buf[MAX_TAG];
	/* выпуск подкаста без тегов: «2026-10-15 - Название» — дата видна в подписи, в названии не нужна */
	int dated = is_podcast(path) && strlen(name) > 13 && isdigit((unsigned char)name[0]) && name[4] == '-' &&
		    !strncmp(name + 10, " - ", 3);
	if (!t.title[0])
		title_from_name(dated ? name + 13 : name, buf);
	tr->path = strdup(path);
	tr->title = xstrdup(t.title[0] ? t.title : buf);
	tr->artist = xstrdup(t.artist[0] ? t.artist : "Неизвестный исполнитель");
	tr->album = xstrdup(t.album);
	tr->albumartist = xstrdup(t.albumartist);
	tr->track = t.track;
	tr->disc = t.disc;
	tr->year = t.year;
	tr->dur = t.dur;
	tr->pic_off = t.pic_off;
	tr->pic_len = t.pic_len;
	tr->rg_track = t.rg_track;
	tr->rg_album = t.rg_album;
	tr->rg_tpeak = t.rg_tpeak;
	tr->rg_apeak = t.rg_apeak;
}

/* ---------- CUE ---------- */

#define CUE_MAX 99

typedef struct {
	char file[256], title[MAX_TAG], perf[MAX_TAG];
	int year, nfiles, n;
	float rg_album, rg_apeak;
	struct {
		int num;
		char title[MAX_TAG], perf[MAX_TAG];
		double start;
		float rg, peak;
	} tr[CUE_MAX];
} cue_t;

/* значение после ключевого слова: в кавычках или до конца строки; кодировка — как у тегов */
static void cue_value(const char *s, char *out)
{
	while (*s == ' ' || *s == '\t')
		s++;
	char raw[MAX_TAG];
	int n = 0;
	if (*s == '"') {
		s++;
		while (*s && *s != '"' && n < MAX_TAG - 1)
			raw[n++] = *s++;
	} else {
		while (*s && n < MAX_TAG - 1)
			raw[n++] = *s++;
	}
	raw[n] = 0;
	bytes_to_utf8((const unsigned char *)raw, n, out, MAX_TAG);
	trim(out);
}

static int cue_parse(const char *path, cue_t *c)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	memset(c, 0, sizeof(*c));
	c->rg_album = c->rg_apeak = LIB_RG_NONE;
	char line[1024];
	int cur = -1;
	while (fgets(line, sizeof(line), f)) {
		char *s = line;
		if (!memcmp(s, "\xef\xbb\xbf", 3))
			s += 3;
		s[strcspn(s, "\r\n")] = 0;
		while (*s == ' ' || *s == '\t')
			s++;
		if (!strncasecmp(s, "FILE ", 5)) {
			c->nfiles++;
			char v[MAX_TAG];
			/* имя в кавычках, после — тип (WAVE/MP3); без кавычек — до последнего пробела */
			const char *q = s + 5;
			while (*q == ' ')
				q++;
			if (*q == '"') {
				cue_value(q, v);
			} else {
				snprintf(v, sizeof(v), "%s", q);
				char *sp = strrchr(v, ' ');
				if (sp)
					*sp = 0;
			}
			snprintf(c->file, sizeof(c->file), "%s", v);
		} else if (!strncasecmp(s, "TRACK ", 6)) {
			if (c->n < CUE_MAX && strstr(s, "AUDIO")) {
				cur = c->n++;
				c->tr[cur].num = atoi(s + 6);
				c->tr[cur].start = -1;
				c->tr[cur].rg = c->tr[cur].peak = LIB_RG_NONE;
			} else {
				cur = -2;   /* не аудио — пропуск */
			}
		} else if (!strncasecmp(s, "TITLE ", 6)) {
			cue_value(s + 6, cur >= 0 ? c->tr[cur].title : c->title);
		} else if (!strncasecmp(s, "PERFORMER ", 10)) {
			cue_value(s + 10, cur >= 0 ? c->tr[cur].perf : c->perf);
		} else if (!strncasecmp(s, "REM REPLAYGAIN_", 15)) {
			float v = (float)atof(strchr(s + 15, ' ') ? strchr(s + 15, ' ') : "99");
			int album = !strncasecmp(s + 15, "ALBUM", 5), peak = strstr(s, "PEAK") != NULL;
			if (album)
				*(peak ? &c->rg_apeak : &c->rg_album) = v;
			else if (cur >= 0)
				*(peak ? &c->tr[cur].peak : &c->tr[cur].rg) = v;
		} else if (!strncasecmp(s, "REM DATE ", 9) && cur == -1) {
			c->year = atoi(s + 9);
		} else if (!strncasecmp(s, "INDEX 01 ", 9) && cur >= 0) {
			int m = 0, sec = 0, fr = 0;
			sscanf(s + 9, "%d:%d:%d", &m, &sec, &fr);
			c->tr[cur].start = m * 60 + sec + fr / 75.0;
		}
	}
	fclose(f);
	return c->nfiles == 1 && c->n > 0 && c->file[0];
}

/* аудиофайл, на который ссылается лист: по имени; если расширение другое (образ пережат) — по основе имени */
static int cue_target(const cue_t *c, char **names, int n)
{
	const char *base = strrchr(c->file, '\\') ? strrchr(c->file, '\\') + 1 : c->file;
	for (int i = 0; i < n; i++)
		if (!strcasecmp(names[i], base))
			return i;
	const char *dot = strrchr(base, '.');
	size_t bl = dot ? (size_t)(dot - base) : strlen(base);
	const char *ext;
	for (int i = 0; i < n; i++)
		if (is_audio(names[i], &ext) && !strncasecmp(names[i], base, bl) && names[i][bl] == '.')
			return i;
	return -1;
}

static void add_cue_tracks(const cue_t *c, const char *path, const char *ext, struct stat *st, lib_dir_t *dir)
{
	tags_t t;
	get_tags(path, ext, st, &t);
	drop_url(t.artist);
	drop_url(t.albumartist);
	for (int i = 0; i < c->n; i++) {
		if (c->tr[i].start < 0)
			continue;
		double end = i + 1 < c->n && c->tr[i + 1].start > c->tr[i].start ? c->tr[i + 1].start : 0;
		lib_track_t *tr = new_track(dir);
		char buf[MAX_TAG];
		snprintf(buf, sizeof(buf), "Трек %d", c->tr[i].num);
		tr->path = strdup(path);
		tr->title = xstrdup(c->tr[i].title[0] ? c->tr[i].title : buf);
		tr->artist = xstrdup(c->tr[i].perf[0] ? c->tr[i].perf : c->perf[0] ? c->perf : t.artist[0] ? t.artist : "Неизвестный исполнитель");
		tr->album = xstrdup(c->title[0] ? c->title : t.album);
		tr->albumartist = xstrdup(c->perf[0] ? c->perf : t.albumartist);
		tr->track = c->tr[i].num;
		tr->disc = t.disc;
		tr->year = c->year ? c->year : t.year;
		tr->start = c->tr[i].start;
		tr->end = end;
		tr->dur = (end ? end : t.dur) - c->tr[i].start;
		tr->pic_off = t.pic_off;
		tr->pic_len = t.pic_len;
		tr->rg_track = c->tr[i].rg != LIB_RG_NONE ? c->tr[i].rg : t.rg_track;
		tr->rg_tpeak = c->tr[i].peak != LIB_RG_NONE ? c->tr[i].peak : t.rg_tpeak;
		tr->rg_album = c->rg_album != LIB_RG_NONE ? c->rg_album : t.rg_album;
		tr->rg_apeak = c->rg_apeak != LIB_RG_NONE ? c->rg_apeak : t.rg_apeak;
	}
}

static void walk(const char *path, lib_dir_t *dir)
{
	DIR *d = opendir(path);
	if (!d)
		return;
	char **names = NULL;
	int n = 0;
	struct dirent *e;
	while ((e = readdir(d))) {
		if (skip_dir(e->d_name))
			continue;
		/* тип — первым байтом имени: папку видно без stat, stat нужен только звуку */
		names = realloc(names, (n + 1) * sizeof(char *));
		names[n] = malloc(strlen(e->d_name) + 2);
		names[n][0] = e->d_type == DT_DIR ? 'd' : e->d_type == DT_UNKNOWN ? '?' : 'f';
		strcpy(names[n] + 1, e->d_name);
		n++;
	}
	closedir(d);
	qsort(names, n, sizeof(char *), name_cmp);
	char kind[n ? n : 1];
	for (int i = 0; i < n; i++) {
		kind[i] = names[i][0];
		memmove(names[i], names[i] + 1, strlen(names[i]));
	}
	char full[1024];
	cue_t **cue_of = calloc(n ? n : 1, sizeof(cue_t *));   /* лист для файла-образа */
	for (int i = 0; i < n; i++) {
		const char *dot = strrchr(names[i], '.');
		if (!dot || strcasecmp(dot, ".cue"))
			continue;
		cue_t *c = malloc(sizeof(cue_t));
		snprintf(full, sizeof(full), "%s/%s", path, names[i]);
		int k = c && cue_parse(full, c) ? cue_target(c, names, n) : -1;
		if (k >= 0 && !cue_of[k])
			cue_of[k] = c;
		else
			free(c);
	}
	for (int i = 0; i < n; i++) {
		snprintf(full, sizeof(full), "%s/%s", path, names[i]);
		struct stat st;
		const char *ext;
		int audio = is_audio(names[i], &ext);
		if (kind[i] == '?' || audio)
			if (stat(full, &st))
				kind[i] = 'x';
		if (kind[i] == 'd' || (kind[i] == '?' && S_ISDIR(st.st_mode))) {
			lib_dir_t sub = { strdup(names[i]), NULL, 0, NULL, 0 };
			walk(full, &sub);
			if (sub.ndirs || sub.ntracks) {
				dir->dirs = realloc(dir->dirs, (dir->ndirs + 1) * sizeof(lib_dir_t));
				dir->dirs[dir->ndirs++] = sub;
			}
		} else if (!audio && is_image(names[i])) {
			if (nseen == capseen) {
				capseen = capseen ? capseen * 2 : 256;
				seen = realloc(seen, capseen * sizeof(char *));
			}
			seen[nseen++] = strdup(full);
		} else if (audio && kind[i] != 'x') {
			if (cue_of[i])
				add_cue_tracks(cue_of[i], full, ext, &st, dir);
			else
				add_track(full, names[i], ext, &st, dir);
		}
		free(cue_of[i]);
		free(names[i]);
	}
	free(cue_of);
	free(names);
}

/* ---------- альбомы, исполнители ---------- */

/* основной исполнитель: «Gorillaz feat. Miho Hatori», «A ft. B», «A × B», «A (feat. B)» → «Gorillaz», «A».
 * По нему группируются альбомы и список исполнителей; у трека остаётся полное имя */
static const char *primary_artist(const char *s)
{
	static const char *const seps[] = { " feat. ", " feat ", " ft. ", " ft ", " featuring ", " (feat", " [feat", " (ft.", " (with ",
					    " × ", " x ", " vs. ", " vs ", NULL };
	static char out[MAX_TAG];
	char low[MAX_TAG];
	snprintf(out, sizeof(out), "%s", s);
	int n = 0;
	for (; out[n] && n < MAX_TAG - 1; n++)
		low[n] = (char)tolower((unsigned char)out[n]);
	low[n] = 0;
	for (int i = 0; seps[i]; i++) {
		char *f = strstr(low, seps[i]);
		if (f && f > low) {
			out[f - low] = 0;
			low[f - low] = 0;
		}
	}
	trim(out);
	return out[0] ? out : s;
}

/* ссылка вместо исполнителя (так подписывают раздачи) — считается пустым тегом */
static void drop_url(char *s)
{
	if (!strncasecmp(s, "http", 4) || !strncasecmp(s, "www.", 4) || strstr(s, "://"))
		s[0] = 0;
}

static char *dir_of(const char *path)
{
	char *d = strdup(path), *s = strrchr(d, '/');
	if (s)
		*s = 0;
	/* «Disc 1», «CD2», «Диск 1» — часть альбома: папка альбома выше */
	s = strrchr(d, '/');
	const char *last = s ? s + 1 : d;
	if (s && (!strncasecmp(last, "disc", 4) || !strncasecmp(last, "cd", 2) || !strncmp(last, "Диск", 8)))
		*s = 0;
	return d;
}

/* оценка имени картинки: обложка — выше, задник, вкладыш, диск, фото группы — не обложка */
static int cover_score(const char *name)
{
	static const char *const bad[] = { "back", "insert", "inlay", "inside", "booklet", "gatefold", "tray", "disc",
					   "cd", "band", "artist", "label", "matrix", "obi", "spine", "media", NULL };
	char low[256];
	int n = 0;
	for (; name[n] && n < 255; n++)
		low[n] = (char)tolower((unsigned char)name[n]);
	low[n] = 0;
	char *dot = strrchr(low, '.');
	if (dot)
		*dot = 0;
	for (int i = 0; bad[i]; i++)
		if (strstr(low, bad[i]))
			return -1;
	if (!strcmp(low, "folder") || !strcmp(low, "cover") || !strcmp(low, "front") || !strcmp(low, "albumart"))
		return 40;
	if (strstr(low, "front"))
		return 30;
	if (strstr(low, "cover") || strstr(low, "folder"))
		return 20;
	return 0;
}

/* формат и размер по заголовку: 2 — baseline JPEG (любой размер, уменьшается при декодировании),
 * 1 — PNG или прогрессивный JPEG не больше COVER_MAX_PX (декодируются целиком), 0 — не годится */
#define COVER_MAX_PX (2000 * 1000)
static int cover_read(const char *path, long *px);

static int cover_usable(const char *path, long *px)
{
	struct stat st;
	if (stat(path, &st))
		return 0;
	img_t key = { .path = (char *)path }, *m = nimgs ? bsearch(&key, imgs, nimgs, sizeof(*imgs), img_cmp) : NULL;
	int usable;
	if (m && m->size == (long)st.st_size && m->mtime == (long)st.st_mtime) {
		*px = m->px;
		usable = m->usable;
	} else {
		usable = cover_read(path, px);
	}
	img_push(&fresh_imgs, &nfresh_imgs, &capfresh_imgs, (img_t){ strdup(path), (long)st.st_size, (long)st.st_mtime, *px, usable });
	return usable;
}

static int cover_read(const char *path, long *px)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	static unsigned char b[64 * 1024];   /* только поток сканирования */
	int n = (int)fread(b, 1, sizeof(b), f), ok = 0;
	fclose(f);
	*px = 0;
	if (n > 24 && !memcmp(b, "\x89PNG", 4)) {
		*px = (long)be32(b + 16) * be32(b + 20);
		return *px <= COVER_MAX_PX;
	}
	for (int i = 2; n > 4 && b[0] == 0xff && b[1] == 0xd8 && i + 9 < n;) {
		if (b[i] != 0xff)
			break;
		int m = b[i + 1];
		if (m == 0xff) {
			i++;
			continue;
		}
		if (m >= 0xc0 && m <= 0xc2) {
			*px = (long)(b[i + 5] << 8 | b[i + 6]) * (b[i + 7] << 8 | b[i + 8]);
			ok = m == 0xc0 ? 2 : *px <= COVER_MAX_PX;
			break;
		}
		i += 2 + (b[i + 2] << 8 | b[i + 3]);
	}
	return ok;
}

typedef struct {
	char *path;
	int score, usable;
	long px;
} cand_t;

static int str_cmp(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* картинки прямо в папке dir — из списка обхода (он отсортирован: они идут подряд за «dir/») */
static void add_cands(const char *dir, cand_t **c, int *n)
{
	size_t dl = strlen(dir);
	int lo = 0, hi = nseen;
	while (lo < hi) {
		int mid = (lo + hi) / 2;
		int r = strncmp(seen[mid], dir, dl);
		if (r < 0 || (r == 0 && seen[mid][dl] < '/'))
			lo = mid + 1;
		else
			hi = mid;
	}
	for (int i = lo; i < nseen && !strncmp(seen[i], dir, dl) && seen[i][dl] == '/'; i++) {
		const char *name = seen[i] + dl + 1;
		if (strchr(name, '/'))
			continue;   /* во вложенной папке */
		int s = cover_score(name);
		if (s < 0)
			continue;
		*c = realloc(*c, (*n + 1) * sizeof(cand_t));
		(*c)[(*n)++] = (cand_t){ strdup(seen[i]), s, -1, 0 };
	}
}

/* лучше — по имени; при равенстве — то, что декодируется дёшево, и что меньше (но не мельче 250 px) */
static int cand_cmp(const void *pa, const void *pb)
{
	const cand_t *a = pa, *b = pb;
	if (a->score != b->score)
		return b->score - a->score;
	if (a->usable != b->usable)
		return b->usable - a->usable;
	int sa = a->px >= 250 * 250, sb = b->px >= 250 * 250;
	if (sa != sb)
		return sb - sa;
	return a->px < b->px ? -1 : a->px > b->px;
}

static char *embedded_cover(const lib_album_t *al)
{
	for (int i = 0; i < al->ntracks; i++) {
		const lib_track_t *t = &L->tracks[al->tracks[i]];
		if (t->pic_len) {
			char buf[1100];
			snprintf(buf, sizeof(buf), "@%ld:%ld:%s", t->pic_off, t->pic_len, t->path);
			return strdup(buf);
		}
	}
	return NULL;
}

/* картинка из папки с именем обложки → из тегов → любая другая картинка из папки */
static char *find_cover(const lib_album_t *al)
{
	static const char *const sub[] = { "", "/Scans", "/Covers", "/Artwork", "/scans", "/covers", "/artwork" };
	cand_t *c = NULL;
	int n = 0;
	char p[1024];
	for (unsigned i = 0; i < sizeof(sub) / sizeof(sub[0]); i++) {
		snprintf(p, sizeof(p), "%s%s", al->dir, sub[i]);
		add_cands(p, &c, &n);
	}
	for (int i = 0; i < n; i++)
		c[i].usable = cover_usable(c[i].path, &c[i].px);
	qsort(c, n, sizeof(cand_t), cand_cmp);
	char *res = NULL;
	for (int i = 0; i < n && !res; i++)
		if (c[i].score >= 20 && c[i].usable)
			res = strdup(c[i].path);
	if (!res)
		res = embedded_cover(al);
	for (int i = 0; i < n && !res; i++)
		if (c[i].usable)
			res = strdup(c[i].path);
	for (int i = 0; i < n; i++)
		free(c[i].path);
	free(c);
	return res;
}

static char pod_prefix[320];   /* «корень/Podcasts/» */

static int is_podcast(const char *path)
{
	return !strncmp(path, pod_prefix, strlen(pod_prefix));
}

static int cmp_track_in_album(const void *a, const void *b)
{
	const lib_track_t *x = &L->tracks[*(const int *)a], *y = &L->tracks[*(const int *)b];
	if (is_podcast(x->path))   /* имена выпусков начинаются с даты — новые сверху */
		return strcmp(y->path, x->path);
	if (x->disc != y->disc) return x->disc - y->disc;
	if (x->track != y->track) return x->track - y->track;
	return strcmp(x->path, y->path);
}

static const char *album_artist(const lib_album_t *a)
{
	return a->artist;
}

static int cmp_album(const void *pa, const void *pb)
{
	const lib_album_t *a = &L->albums[*(const int *)pa], *b = &L->albums[*(const int *)pb];
	int c = lib_strcmp(album_artist(a), album_artist(b));
	if (c) return c;
	if (a->year != b->year) return a->year - b->year;
	return lib_strcmp(a->title, b->title);
}

static int cmp_all(const void *pa, const void *pb)
{
	const lib_track_t *x = &L->tracks[*(const int *)pa], *y = &L->tracks[*(const int *)pb];
	if (x->aidx != y->aidx) {
		int ka = x->aidx, kb = y->aidx;
		return cmp_album(&ka, &kb);
	}
	return cmp_track_in_album(pa, pb);
}

static void build_albums(void)
{
	for (int i = 0; i < L->ntracks; i++) {
		lib_track_t *t = &L->tracks[i];
		char *dir = dir_of(t->path);
		char prim[MAX_TAG], aa[MAX_TAG];
		snprintf(prim, sizeof(prim), "%s", primary_artist(t->artist));
		snprintf(aa, sizeof(aa), "%s", t->albumartist[0] ? primary_artist(t->albumartist) : prim);
		int found = -1, pod = is_podcast(t->path);   /* подкаст — вся его папка, что бы ни было в тегах выпусков */
		for (int a = L->nalbums - 1; a >= 0 && found < 0; a--) {
			lib_album_t *al = &L->albums[a];
			if (pod || al->podcast ? pod && al->podcast && !strcmp(al->dir, dir)
			    : t->album[0] ? (!strcasecmp(al->title, t->album) && (!strcasecmp(al->artist, aa) || !strcmp(al->dir, dir)))
					  : !strcmp(al->dir, dir) && !strcmp(al->title, strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir))
				found = a;
		}
		if (found < 0) {
			L->albums = realloc(L->albums, (L->nalbums + 1) * sizeof(lib_album_t));
			lib_album_t *al = &L->albums[L->nalbums];
			memset(al, 0, sizeof(*al));
			al->title = strdup(t->album[0] && !pod ? t->album : (strrchr(dir, '/') ? strrchr(dir, '/') + 1 : dir));
			al->artist = strdup(pod ? "Подкасты" : aa);
			al->dir = dir;
			al->year = t->year;
			al->podcast = pod;
			found = L->nalbums++;
		} else {
			lib_album_t *al = &L->albums[found];
			/* у сборников разные исполнители треков, а тега исполнителя альбома нет */
			if (!pod && !t->albumartist[0] && lib_strcmp(al->artist, prim) && strcmp(al->artist, "Разные исполнители")) {
				free(al->artist);
				al->artist = strdup("Разные исполнители");
			}
			free(dir);
		}
		lib_album_t *al = &L->albums[found];
		al->tracks = realloc(al->tracks, (al->ntracks + 1) * sizeof(int));
		al->tracks[al->ntracks++] = i;
		t->aidx = found;
	}
	for (int a = 0; a < L->nalbums; a++) {
		lib_album_t *al = &L->albums[a];
		qsort(al->tracks, al->ntracks, sizeof(int), cmp_track_in_album);
		al->cover = find_cover(al);
	}
}

/* «Various», «VA» — сборник */
static int is_various(const char *s)
{
	return !strcasecmp(s, "Various") || !strcasecmp(s, "Various Artists") || !strcasecmp(s, "VA") ||
	       !strcasecmp(s, "V.A.");
}

/* совместные релизы «Woodkid & Nils Frahm», «Digitalism And Fedde Le Grand»: к первому исполнителю, если он
 * в библиотеке есть сам по себе (иначе «Simon & Garfunkel» развалился бы) */
static void normalize_album_artists(void)
{
	static const char *const seps[] = { " & ", " and ", ", ", " + ", NULL };
	for (int a = 0; a < L->nalbums; a++)
		if (is_various(L->albums[a].artist)) {
			free(L->albums[a].artist);
			L->albums[a].artist = strdup("Разные исполнители");
		}
	for (int a = 0; a < L->nalbums; a++) {
		lib_album_t *al = &L->albums[a];
		char low[MAX_TAG];
		int n = 0;
		for (; al->artist[n] && n < MAX_TAG - 1; n++)
			low[n] = (char)tolower((unsigned char)al->artist[n]);
		low[n] = 0;
		for (int i = 0; seps[i]; i++) {
			char *f = strstr(low, seps[i]);
			if (!f || f == low)
				continue;
			char head[MAX_TAG];
			snprintf(head, sizeof(head), "%.*s", (int)(f - low), al->artist);
			int known = 0;
			for (int b = 0; b < L->nalbums && !known; b++)
				known = b != a && !lib_strcmp(L->albums[b].artist, head);
			if (known) {
				free(al->artist);
				al->artist = strdup(head);
				break;
			}
		}
	}
}

static void build_artists(void)
{
	normalize_album_artists();
	int *order = malloc(L->nalbums * sizeof(int));
	for (int a = 0; a < L->nalbums; a++)
		order[a] = a;
	qsort(order, L->nalbums, sizeof(int), cmp_album);
	for (int k = 0; k < L->nalbums; k++) {
		lib_album_t *al = &L->albums[order[k]];
		if (al->podcast)
			continue;
		if (!L->nartists || lib_strcmp(L->artists[L->nartists - 1].name, al->artist)) {
			L->artists = realloc(L->artists, (L->nartists + 1) * sizeof(lib_artist_t));
			L->artists[L->nartists++] = (lib_artist_t){ strdup(al->artist), NULL, 0 };
		}
		lib_artist_t *ar = &L->artists[L->nartists - 1];
		ar->albums = realloc(ar->albums, (ar->nalbums + 1) * sizeof(int));
		ar->albums[ar->nalbums++] = order[k];
	}
	free(order);
	L->by_title = malloc((L->ntracks ? L->ntracks : 1) * sizeof(int));
	L->nby_title = 0;
	for (int i = 0; i < L->ntracks; i++)
		if (!L->albums[L->tracks[i].aidx].podcast)
			L->by_title[L->nby_title++] = i;
	qsort(L->by_title, L->nby_title, sizeof(int), cmp_all);
}

library_t *lib_scan(const char *root, const char *cachefile, volatile int *progress)
{
	L = calloc(1, sizeof(*L));
	prog = progress;
	snprintf(pod_prefix, sizeof(pod_prefix), "%s/%s/", root, LIB_PODCASTS);
	captracks = 0;
	ncache = nfresh = nimgs = nfresh_imgs = nseen = 0;
	cache_load(cachefile);
	L->root.name = strdup("SD-карта");
	walk(root, &L->root);
	qsort(seen, nseen, sizeof(char *), str_cmp);
	build_albums();
	cache_save(cachefile);   /* после альбомов: в кэш идут и проверенные картинки */
	build_artists();
	return L;
}
