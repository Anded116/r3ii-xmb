#ifndef XMB_SUBPROC_H
#define XMB_SUBPROC_H

#include <stdio.h>

/* popen/system без наследства: дочерний процесс не получает открытые файлы XMB. Иначе долгоживущий потомок
 * (wpa_supplicant -B, adbd, bluealsa-cli) держит звуковой PCM, кадровый буфер, файлы на карте — и PCM потом не
 * открыть («Resource busy»), а карту не отмонтировать */
FILE *sp_popen(const char *cmd, const char *mode);   /* "r" или "w" */
int sp_pclose(FILE *f);
int sp_system(const char *cmd);
/* xmbget (скачивание по HTTPS, рядом — cacert.pem): /tmp/xmb, внутренняя память или карта; NULL — нет */
const char *sp_xmbget(void);

#endif
