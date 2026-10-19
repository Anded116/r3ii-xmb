#ifndef XMB_BT_H
#define XMB_BT_H

/* Bluetooth через стоковые BlueZ (bluetoothctl) и BlueALSA: всё медленное — в фоновом потоке, интерфейс берёт
 * снимок. Подключённые наушники сами становятся выходом звука (audio_set_bt), отключились — снова разъём. */

#define BT_MAX 24

typedef struct {
	char mac[18], name[64];
	int paired, connected;
} bt_dev_t;

void bt_init(int on);
void bt_power(int on);
/* перед сном (1): Bluetooth-половина чипа BCM43430 — тот же модуль, что Wi-Fi на SDIO — обесточена, как у стока и
 * Compás (bt_suspend: hci0 down, rfkill); −1 — не дождались. После (0): включён в настройках — поднимается в фоне
 * (bt_resume, ~10 с), выключен — остаётся обесточенным до включения */
int bt_park(int park);
void bt_scan(void);                       /* поиск ~10 с: найденные появятся в списке */
void bt_connect(const char *mac);         /* сопряжение (если нужно), доверие, подключение */
void bt_disconnect(const char *mac);
void bt_set_codec(const char *name);      /* кодек подключённых: SBC, AAC, aptX, aptX-HD, LDAC; NULL — как есть */
int bt_devices(bt_dev_t *out, int max);   /* снимок: подключённые, потом сопряжённые, потом найденные */
unsigned bt_gen(void);                    /* растёт при любой перемене списка или состояния */
const char *bt_status(void);              /* что идёт сейчас: «поиск…», «подключение…», пусто — ничего */
const char *bt_message(unsigned *seq);    /* итог последней операции для подсказки; seq растёт с каждым */

#endif
