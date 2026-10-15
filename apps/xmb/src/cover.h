#ifndef COVER_H
#define COVER_H

#include "lvgl/lvgl.h"

/* Обложки: JPEG → квадрат size×size RGB565, декодирование в фоновом потоке, кэш. */

void cover_init(const char *cache_dir);   /* cache_dir — миниатюры на карте, NULL — без кэша */
/* готовая обложка или NULL (тогда запрос поставлен в очередь, файла нет или формат не поддержан) */
const lv_image_dsc_t *cover_get(const char *path, int size);
/* растёт, когда готова новая обложка — UI перерисовывает места, где ждал */
unsigned cover_generation(void);
/* после cover_get: 1 — готова, 0 — декодируется, -1 — нет (файла нет, формат не поддержан) */
int cover_ready(const char *path, int size);
/* 1 — не брать новых заданий и дождаться текущего (карта уходит компьютеру), 0 — продолжить */
void cover_hold(int hold);

#endif
