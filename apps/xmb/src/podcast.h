#ifndef XMB_PODCAST_H
#define XMB_PODCAST_H

/* Подкасты: подписки выбираются на компьютере — ссылки на RSS в Podcasts/подписки.txt (по одной в строке, # —
 * комментарий) или экспорт .opml в Podcasts/. Плеер по Wi-Fi сам докачивает свежие выпуски в
 * Podcasts/<подкаст>/<дата> - <выпуск>.<mp3|m4a> с обложкой cover.jpg; прослушанные сверх POD_KEEP удаляет.
 * Где остановился и «прослушан» — в Podcasts/.state. */

#define POD_KEEP 3   /* свежих выпусков на подкаст */

void pod_init(const char *music_root);
void pod_tick(int online);        /* раз в кадр: при Wi-Fi и пора — проверить подписки в фоне */
void pod_update(void);            /* проверить сейчас (кнопка «Обновить») */
int pod_busy(void);
const char *pod_status(void);     /* что идёт: «проверка подписок…», «скачивается 2 из 3…»; пусто — ничего */
const char *pod_message(unsigned *seq);
int pod_take_new(void);           /* скачано выпусков с прошлого вызова — библиотеке перечитаться */
int pod_feeds(void);              /* подписок в файлах */

/* где остановился (секунды, 0 — с начала) и прослушан ли выпуск; path — файл выпуска */
double pod_pos(const char *path);
int pod_done(const char *path);
void pod_save(const char *path, double pos, int done);

#endif
