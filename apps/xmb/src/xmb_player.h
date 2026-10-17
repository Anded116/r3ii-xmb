#ifndef XMB_PLAYER_H
#define XMB_PLAYER_H

#define PLAYER_DIM 170   /* затемнение фона под раскрытым плеером */

#include "lvgl/lvgl.h"
#include "lyrics.h"

typedef struct {
	const char *title, *artist, *album, *format;
	int duration;   /* секунды */
} track_t;

/* режимы воспроизведения в ряду под прогрессом */
enum { PM_SHUFFLE, PM_REPEAT, PM_LYRICS, PM_MORE, PM_COUNT };   /* частое — на экране, редкое — в «Ещё» */

void player_create(lv_obj_t *parent, const lv_font_t *f_big, const lv_font_t *f_mid, const lv_font_t *f_small);
void player_set_track(const track_t *t);
void player_set_state(int playing, double pos, int volume);
void player_set_modes(const int on[PM_COUNT], const char *const label[PM_COUNT]);
int player_mode_at(int x, int y);   /* индекс режима под точкой тапа или -1 */
int player_bar_at(int x, int y, double *frac);   /* тап по полосе прогресса: 1 и доля трека */
enum { PA_NONE, PA_FAV, PA_ADD, PA_ALBUM };   /* PA_ALBUM — тап по названию: открыть альбом */
int player_action_at(int x, int y);   /* сердечко (избранное) или «+» (в плейлист) под тапом */
void player_set_fav(int on);
void player_set_ab(double a, double b);   /* отметки повтора A-B на полосе: доли трека, < 0 — нет */
void player_set_icon_font(const lv_font_t *f);   /* Font Awesome — для сердечка */
double player_bar_frac(int x);                   /* доля трека под пальцем, пока его тянут по полосе */
void player_show_volume(void);
/* текст песни на месте обложки: строки живут, пока не придёт новый (NULL — нет); текущая — по позиции трека */
void player_set_lyrics(const lyrics_t *l);
void player_show_lyrics(int on);
int player_lyrics_shown(void);
void player_show(int show);
int player_shown(void);
int player_covers(void);   /* плеер выехал полностью и закрывает меню */
int player_carousel_moving(void);   /* обложки едут (перелистывание альбома) */
double player_progress(void); /* раскрытие из мини-плеера: 0 … 1 */
void player_tick(double dt);

/* карусель альбомов: соседние альбомы (или папки) выглядывают по краям, горизонтальный свайп — переход */
void player_set_neighbors(const char *prev_album, const char *prev_cover, const char *next_album, const char *next_cover);
void player_set_cover(const char *path);   /* обложка вместо кассеты; NULL — кассета */
void player_drag(int dx);          /* палец тянет карусель */
void player_release(int dir);      /* dir = ±1 — перешли на соседний альбом, 0 — вернуть на место */
/* пустая обложка (альбом без картинки): серый квадрат без скругления и серая нота, как в Apple Music */
#define EMPTY_BG 0x3a3a3c
#define EMPTY_FG 0x8e8e93

#endif
