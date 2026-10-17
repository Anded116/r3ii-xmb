#ifndef XMB_SPAWN_H
#define XMB_SPAWN_H

#include <stdio.h>

/* popen/system без наследства: дочерний процесс не получает открытые файлы XMB. Иначе долгоживущий потомок
 * (wpa_supplicant -B, adbd, bluealsa-cli) держит звуковой PCM, кадровый буфер, файлы на карте — и PCM потом не
 * открыть («Resource busy»), а карту не отмонтировать */
FILE *sp_popen(const char *cmd, const char *mode);   /* "r" или "w" */
int sp_pclose(FILE *f);
int sp_system(const char *cmd);

#endif
