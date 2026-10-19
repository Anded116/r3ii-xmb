#ifndef XMB_WAVE_MODULE_H
#define XMB_WAVE_MODULE_H

/*
 * Волна фона XMB — сменный модуль. Рисует полосу WAVE_W × WAVE_H пикселей RGB565 поверх фона
 * (градиент темы), которую интерфейс выводит на экран с y = WAVE_Y, под меню и текстом.
 *
 * Производительность — главное требование (процессор MIPS ~1 ГГц, кадр 22 мс на всё):
 *  - render() вызывается каждый кадр (45 раз в секунду) и должен укладываться в ~1 мс на плеере
 *    (на ПК это порядка 0.05–0.1 мс; стенд tools/wavetest меряет);
 *  - менять только то, что изменилось: вне прямоугольников из dirty пиксели dst обязаны остаться
 *    ровно такими, как после прошлого вызова; интерфейс перерисует только эти прямоугольники
 *    (и всё, что лежит поверх них), поэтому чем их меньше по площади — тем лучше;
 *  - первый вызов после init() или после смены фона (full = 1) — перерисовать всю полосу и
 *    вернуть один прямоугольник на всю полосу.
 */
#include <stdint.h>

#define WAVE_W 320
#define WAVE_H 152
#define WAVE_Y 214
#define WAVE_BANDS 32           /* полос спектра: 40 Гц … 16 кГц, логарифмически, басы — первые */
#define WAVE_MAX_DIRTY 24

typedef struct {
	int x1, y1, x2, y2;     /* включительно, в координатах полосы */
} wave_rect_t;

typedef struct {
	int n;
	wave_rect_t r[WAVE_MAX_DIRTY];
} wave_dirty_t;

/* музыка глазами фона (wave_music.c): всё — относительно недавнего прошлого этого же трека, поэтому тихая запись и
 * громкий мастеринг двигают картинку одинаково; 0 в тишине и на паузе */
typedef struct {
	float band[WAVE_BANDS];         /* 0..1: полоса относительно своего размаха за последние секунды */
	float bass, mid, treb;          /* 0..1: группы полос (40–150 Гц, 150 Гц – 2 кГц, 2–16 кГц), быстрые */
	float bass_att, mid_att, treb_att;   /* они же, сглаженные (~0,5 с) — для размаха и скорости */
	float energy;                   /* 0..1: среднее сглаженных — «насколько плотно играет» */
	float kick, snare, hat;         /* удары: в момент удара — его сила 0,4..1, потом спадает (бочка ~0,2 с) */
	unsigned kicks, snares, hats;   /* счётчики ударов: новый удар — счётчик сменился */
} wave_music_t;

/* обновить по кадру: raw — спектр как есть (без сглаживания, 0..1 = −60…0 дБ), dt — с прошлого кадра */
void wave_music_update(wave_music_t *m, const float *raw, float dt);

/* импульсы бочки, бегущие по полосе слева направо: новый — на каждый удар, сила — сила удара, гаснут за ~1 с;
 * профиль — крутой фронт и длинный хвост. col — сумма по столбцам, 0..256 */
#define WAVE_NPULSE 4
typedef struct {
	float x[WAVE_NPULSE], a[WAVE_NPULSE];
	unsigned last;
} wave_pulses_t;
void wave_pulses_update(wave_pulses_t *p, const wave_music_t *m, float dt, int *col);

typedef struct {
	double t;                       /* секунды с запуска, монотонно */
	double dt;                      /* с прошлого кадра */
	const float *spec;              /* WAVE_BANDS значений 0..1, уже сглажены (атака быстрая, спад медленный) */
	float level;                    /* общий уровень 0..1; 0 и нулевой спектр — музыка на паузе */
	int full;                       /* 1 — перерисовать всё (первый кадр, сменился фон) */
	const wave_music_t *m;          /* разбор музыки; NULL — нет (стенд без него) */
} wave_frame_t;

typedef struct {
	const char *name;               /* для меню настроек, по-русски, коротко */
	void (*init)(void);
	/* bg — фон под полосой (WAVE_W × WAVE_H, RGB565, только чтение); dst — полоса на экран */
	void (*render)(uint16_t *dst, const uint16_t *bg, const wave_frame_t *f, wave_dirty_t *dirty);
} wave_t;

/* добавить прямоугольник (с обрезкой по полосе; при переполнении — объединяет с последним) */
static inline void wave_dirty_add(wave_dirty_t *d, int x1, int y1, int x2, int y2)
{
	if (x1 < 0) x1 = 0;
	if (y1 < 0) y1 = 0;
	if (x2 > WAVE_W - 1) x2 = WAVE_W - 1;
	if (y2 > WAVE_H - 1) y2 = WAVE_H - 1;
	if (x1 > x2 || y1 > y2)
		return;
	if (d->n == WAVE_MAX_DIRTY) {
		wave_rect_t *r = &d->r[d->n - 1];
		r->x1 = x1 < r->x1 ? x1 : r->x1;
		r->y1 = y1 < r->y1 ? y1 : r->y1;
		r->x2 = x2 > r->x2 ? x2 : r->x2;
		r->y2 = y2 > r->y2 ? y2 : r->y2;
		return;
	}
	d->r[d->n++] = (wave_rect_t){ x1, y1, x2, y2 };
}

/* осветлить пиксель RGB565 к белому на a/256 (a 0..256) — как у существующих волн */
static inline uint16_t wave_lighten(uint16_t p, int a)
{
	if (a <= 0)
		return p;
	if (a > 256)
		a = 256;
	int r = p >> 11, g = (p >> 5) & 63, b = p & 31;
	r += ((31 - r) * a) >> 8;
	g += ((63 - g) * a) >> 8;
	b += ((31 - b) * a) >> 8;
	return (uint16_t)((r << 11) | (g << 5) | b);
}

#endif
