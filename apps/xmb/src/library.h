#ifndef LIBRARY_H
#define LIBRARY_H

/* Музыкальная библиотека: обход карты, теги FLAC/MP3, альбомы, исполнители, дерево папок, кэш. */

typedef struct {
	char *path, *title, *artist, *album, *albumartist;
	int track, disc, year;
	double dur;
	long pic_off, pic_len;   /* картинка в тегах файла (0 — нет) */
	double start, end;       /* трек из CUE: границы внутри файла, секунды (end = 0 — до конца) */
	float rg_track, rg_album, rg_tpeak, rg_apeak;   /* ReplayGain: дБ и пики; LIB_RG_NONE — тега нет */
	int aidx;           /* индекс альбома */
} lib_track_t;

typedef struct {
	char *title, *artist, *dir;   /* dir — папка альбома: там ищется обложка */
	int year;
	int *tracks, ntracks;         /* по порядку диск/трек */
	char *cover;                  /* файл обложки, «@смещение:длина:файл» (из тегов) или NULL */
} lib_album_t;

typedef struct {
	char *name;
	int *albums, nalbums;
} lib_artist_t;

typedef struct lib_dir {
	char *name;
	struct lib_dir *dirs;
	int ndirs;
	int *tracks, ntracks;
} lib_dir_t;

typedef struct {
	lib_track_t *tracks;
	int ntracks;
	lib_album_t *albums;
	int nalbums;
	lib_artist_t *artists;
	int nartists;
	int *by_title;        /* все треки: исполнитель → альбом → номер */
	lib_dir_t root;
} library_t;

#define LIB_RG_NONE 99.0f   /* ReplayGain: тега нет */

/* обход root; кэш тегов — в cache (файл), неизменившиеся файлы не перечитываются.
 * progress (может быть NULL) обновляется числом просмотренных файлов */
library_t *lib_scan(const char *root, const char *cache, volatile int *progress);
/* сравнение для сортировки списков: без учёта регистра и для кириллицы, ё = е — буквы указателя идут подряд */
int lib_strcmp(const char *a, const char *b);
/* следующая буква строки в верхнем регистре (латиница, кириллица; ё = е), *p сдвигается — для поиска */
unsigned lib_fold(const char **p);

#endif
