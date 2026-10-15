#ifndef PLAYLIST_H
#define PLAYLIST_H

#include "library.h"

/* Плейлисты — файлы .m3u8 в папке Playlists на карте (пути относительно папки, как у других плееров).
 * Трек из CUE — строка #XMB-START:<секунды> перед путём к образу. Первый плейлист — всегда «Избранное». */

typedef struct {
	char *name, *file;
	int *tracks, n;
} playlist_t;

#define PL_FAVORITES 0

/* загрузить все плейлисты папки dir; треки — индексы библиотеки, отсутствующие на карте пропускаются */
int pl_load_all(const library_t *L, const char *dir, playlist_t **out);
/* новый пустой плейлист «Плейлист N» в конце массива; возвращает его индекс */
int pl_create(playlist_t **pls, int *n, const char *dir);
int pl_find(const playlist_t *p, int track);   /* позиция трека или -1 */
void pl_add(playlist_t *p, int track);
void pl_remove(playlist_t *p, int pos);
int pl_save(const library_t *L, const playlist_t *p, const char *root);
void pl_move(playlist_t *p, int pos, int dir);   /* трек на одну позицию выше (−1) или ниже (+1) */
/* новое имя: файл переименовывается; −1 — имя занято другим плейлистом */
int pl_rename(playlist_t *pls, int n, int i, const char *dir, const char *name);
void pl_delete(playlist_t **pls, int *n, int i);   /* файл удаляется с карты */

#endif
