#ifndef XMB_WIFI_H
#define XMB_WIFI_H

/* Wi-Fi через стоковые wpa_supplicant / wpa_cli / udhcpc, конфиг общий со стоком (/data/wpa_supplicant.conf):
 * сети, сохранённые там, подключаются сами. Всё медленное — в фоновом потоке, интерфейс берёт снимок. */

#define WIFI_MAX 24

typedef struct {
	char ssid[33];
	int signal;      /* дБм */
	int secured, saved, connected;
} wifi_net_t;

void wifi_init(int on);
void wifi_power(int on);
/* выключить радио перед сном (1) и вернуть после (0). 1: −1 — драйвер не ответил (Wi-Fi завис после сна): так до
 * перезагрузки, в mem с ним нельзя — плеер виснет насовсем; −2 — ещё поднимается после прошлого сна: пропустить
 * этот сон */
int wifi_park(int park);
void wifi_scan(void);
void wifi_connect(const char *ssid, const char *psk);   /* psk = NULL — сохранённая или открытая сеть */
void wifi_forget(const char *ssid);
int wifi_networks(wifi_net_t *out, int max);   /* подключённая, сохранённые, остальные — по силе сигнала */
int wifi_online(void);                         /* подключено и есть адрес */
unsigned wifi_gen(void);
const char *wifi_status(void);                 /* «поиск сетей…», «подключение…», пусто */
const char *wifi_message(unsigned *seq);

#endif
