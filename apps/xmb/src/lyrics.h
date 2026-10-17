#ifndef XMB_LYRICS_H
#define XMB_LYRICS_H

/* Тексты песен: LRC с метками времени ([мм:сс.сс]строка) или простой текст. Файл — рядом с треком:
 * «трек.lrc»; у трека из CUE-образа (весь альбом в одном файле) — «Исполнитель - Название.lrc» в его папке. */

typedef struct {
	double t;           /* секунды; у текста без меток — 0 */
	const char *text;
} lyric_t;

typedef struct {
	lyric_t *v;
	int n, synced;
	char *buf;          /* строки v[].text указывают сюда */
} lyrics_t;

void lyrics_path(char *out, int cap, const char *audio_path, int cue, const char *artist, const char *title);
int lyrics_parse(lyrics_t *l, const char *text);   /* строк > 0 — есть текст */
int lyrics_load(lyrics_t *l, const char *lrc_path);
void lyrics_free(lyrics_t *l);
int lyrics_at(const lyrics_t *l, double sec);      /* строка, которая звучит в sec; -1 — ещё ни одной */

#endif
