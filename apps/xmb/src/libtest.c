/* libtest <корень> <кэш> — проверка библиотеки: время сканирования, счётчики, примеры */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "library.h"

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 1;
	double t = now();
	library_t *L = lib_scan(argv[1], argv[2], NULL);
	printf("scan %.2f s: %d tracks, %d albums, %d artists, %d top dirs\n", now() - t, L->ntracks, L->nalbums,
	       L->nartists, L->root.ndirs);
	if (getenv("LIBALBUMS")) {   /* альбомы, в папке которых есть подстрока: название, исполнитель, год, треков */
		for (int a = 0; a < L->nalbums; a++)
			if (strstr(L->albums[a].dir, getenv("LIBALBUMS")))
				printf("%s | %s | %d | %d | %s\n", L->albums[a].title, L->albums[a].artist, L->albums[a].year,
				       L->albums[a].ntracks, L->albums[a].dir);
		return 0;
	}
	if (getenv("LIBCOVERS")) {   /* все обложки — сравнить выбор между версиями */
		for (int a = 0; a < L->nalbums; a++)
			printf("%s|%s\n", L->albums[a].dir, L->albums[a].cover ? L->albums[a].cover : "-");
		return 0;
	}
	int covers = 0;
	for (int a = 0; a < L->nalbums; a++)
		covers += L->albums[a].cover != NULL;
	printf("albums with cover: %d\n", covers);
	for (int a = 0; a < L->nalbums; a += L->nalbums / 12 + 1) {
		lib_album_t *al = &L->albums[a];
		lib_track_t *tr = &L->tracks[al->tracks[0]];
		printf("- %s — %s (%d), %d tr, cover=%s\n    first: %d. %s [%.0f s]\n", al->artist, al->title, al->year,
		       al->ntracks, al->cover ? "yes" : "no", tr->track, tr->title, tr->dur);
	}
	if (argc > 3)   /* режим covers: файл обложки (или папка без неё) для каждого альбома */
		for (int a = 0; a < L->nalbums; a++)
			printf("COVER\t%s\t%s\n", L->albums[a].cover ? L->albums[a].cover : "-", L->albums[a].dir);
	for (int i = 0; i < L->nartists && i < 40; i++)
		printf("%s%s", i ? " | " : "artists: ", L->artists[i].name);
	printf("\n");
	return 0;
}
