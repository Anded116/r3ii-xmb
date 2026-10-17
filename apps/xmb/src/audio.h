#ifndef AUDIO_H
#define AUDIO_H

/* Аудиодвижок: декодирование (FLAC/MP3/WAV/M4A) и вывод в отдельном потоке; следующий трек — без паузы или с кроссфейдом. */

#define AUDIO_BANDS 32   /* полос спектра: логарифмически 40 Гц … 16 кГц */

typedef struct {
	int playing;        /* идёт воспроизведение (не пауза и не конец) */
	int ended;          /* трек доиграл до конца — UI решает, что дальше */
	double pos, dur;    /* секунды */
	int rate, bits, channels;
	char codec[8];
	float level;               /* 0..1 — уровень сигнала для визуализатора */
	float spec[AUDIO_BANDS];   /* 0..1: уровень полосы, −60…0 дБ */
	int seq;                   /* растёт, когда движок сам перешёл на трек из audio_set_next */
	int gen;                   /* растёт, когда поток открыл трек из audio_play_range (кодек, частота — уже его) */
} audio_status_t;

int audio_init(void);
/* dur_hint — длительность из библиотеки (для MP3 точная длина без полного прохода по файлу неизвестна) */
int audio_play(const char *path, double dur_hint);
/* трек внутри файла (CUE): с start по end секунд, end = 0 — до конца файла */
/* открывает поток звука (заголовки M4A/FLAC с карты — десятки мс, интерфейс не ждёт); не открылся — ended */
int audio_play_range(const char *path, double start, double end, double dur_hint);
void audio_pause(int pause);
/* перед глубоким сном: закрыть PCM и выключить тракт (после сна HBC3000 обесточен; при следующем
 * воспроизведении порт выставляется заново до открытия PCM) */
void audio_power_down(void);
/* закрыть файлы треков (карта уходит компьютеру): воспроизведение остановлено */
void audio_release(void);
void audio_seek(double sec);
void audio_set_volume(int percent);
/* усиление, как в стоке: 0 — высокое, 1 — среднее (−6 дБ на максимуме), 2 — низкое (−12 дБ) */
void audio_set_gain(int gain);
/* ReplayGain играющего трека и следующего audio_play_range: дБ и пик (0 — неизвестен); выше перегрузки не поднимает */
void audio_set_replaygain(float db, float peak);
/* следующий трек: откроется заранее и заиграет сразу по окончании текущего; rg — его ReplayGain, xfade — разрешить
 * кроссфейд. path = NULL — следующего нет. audio_play_range сбрасывает заказ */
void audio_set_next(const char *path, double start, double end, double dur_hint, float rg_db, float rg_peak, int xfade);
void audio_set_crossfade(int ms);   /* 0 — выкл */
void audio_set_spectrum(int on);    /* считать спектр (при погашенном экране и фону без спектра не нужен) */
void audio_get_status(audio_status_t *st);
/* вывод в Bluetooth: PCM BlueALSA (путь D-Bus), его частота и бит на отсчёт (16, 24 или 32); path = NULL — снова
 * в разъём. Частота файла пересчитывается под частоту кодека, громкость — программно */
void audio_set_bt(const char *path, int rate, int bits);
int audio_supported(const char *path);   /* по расширению */
/* формат файла по заголовку, без воспроизведения: кодек (как в audio_status_t), частота, бит; 0 — прочитан */
int audio_probe(const char *path, char codec[8], int *rate, int *bits);

#define EQ_BANDS 10   /* 31, 62, 125, 250, 500 Гц, 1, 2, 4, 8, 16 кГц */
extern const int eq_freq[EQ_BANDS];
/* эквалайзер: усиление полос в дБ (−12…+12); on = 0 — обход */
void audio_set_eq(int on, const float gain[EQ_BANDS]);

#endif
