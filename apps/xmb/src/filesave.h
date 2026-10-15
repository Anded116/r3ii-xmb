#ifndef XMB_FILESAVE_H
#define XMB_FILESAVE_H

#include <stddef.h>

/* Запись файлов на карту — фоновым потоком: запись и sync на exFAT встают на десятки и сотни миллисекунд, в кадре
 * это рывок. Файл пишется целиком и атомарно (path.tmp → rename); для одного пути берётся последнее содержимое. */
void fsave_init(void);
void fsave(const char *path, const char *data, size_t len);
void fsave_sync(void);    /* sync после уже сделанных напрямую изменений (rename, unlink) — тоже в потоке */
void fsave_flush(void);   /* дождаться, пока всё заказанное записано и сброшено на карту (сон, USB, удаление) */

#endif
