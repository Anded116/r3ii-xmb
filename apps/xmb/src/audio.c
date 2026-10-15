/*
 * Аудиодвижок: декодеры dr_flac / dr_mp3 / dr_wav; M4A — ALAC (mp4.c и декодер из Sonix) или AAC (FAAD2)
 * → 16 бит стерео → вывод.
 *
 * Плеер (XMB_DEVICE): tinyalsa, карта 0 устройство 0. Особенности HiBy R3II 2025 (NOTES.md):
 *  - «Output Port Switch» = 1 включает HBC3000 и выход 3.5 мм; 5 — тракт выключен.
 *    Вывод звука при выключенном тракте роняет ядро, поэтому порт ставится до открытия PCM;
 *  - «Left/Right Playback Volume» — ослабление ES9219 шагом 0.5 дБ: 0 — полная громкость, 255 — тишина;
 *  - первую секунду после старта потока выход приглушён (защита от щелчка) — поток не закрывается
 *    на паузе и между треками с той же частотой, на паузе пишется тишина; после долгой паузы тракт гасится.
 * ПК (XMB_HOST): SDL2 audio, громкость программно.
 */
#include "audio.h"

#include <math.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#include "dr_flac.h"
#include "dr_mp3.h"
#include "dr_wav.h"
#include "alacdec.h"
#include "mp4.h"
#include "neaacdec.h"

#if XMB_DEVICE
#include <tinyalsa/asoundlib.h>
#define PORT_PHONES 1
#define PORT_OFF 5
#define IDLE_POWER_OFF 30.0   /* секунд паузы до выключения тракта */
#else
#include <SDL2/SDL.h>
#endif

#define CHUNK 1024   /* кадров за одну запись */

enum { C_NONE, C_FLAC, C_MP3, C_WAV, C_ALAC, C_AAC };

/* декодер одного трека; их два — текущий и следующий (бесшовный переход и кроссфейд) */
typedef struct {
	int codec, have;
	drflac *flac;
	drmp3 mp3;
	drwav wav;
	mp4_file_t *mp4;              /* M4A: контейнер, декодер, текущий пакет и позиция в нём */
	alacdec_t *alac;
	NeAACDecHandle aac;
	uint32_t au, au_size;
	unsigned char *au_buf;
	int32_t *alac_pcm;
	int16_t *aac_pcm;             /* AAC: пакет, раскодированный в 16 бит стерео */
	int pcm_n, pcm_pos;
	int rate, channels, bits;
	unsigned long long frame, total;
	unsigned long long start_f, end_f;   /* трек внутри файла (CUE): кадры начала и конца, end_f = 0 — до конца */
	char path[1024];
	double dur_hint;
	float rg;                     /* множитель ReplayGain этого трека */
	unsigned char toc[100];       /* MP3: оглавление Xing — доля файла на каждый процент длительности */
	uint32_t toc_bytes;           /* и размер потока по нему; 0 — оглавления нет */
} dec_t;

static int aac_open(dec_t *d);

static struct {
	pthread_t thread;
	pthread_mutex_t mu;
	dec_t d[2];
	int cur;                      /* d[cur] — играет, d[!cur] — следующий */
	/* следующий трек: интерфейс заказывает заранее, поток открывает к концу текущего */
	char nx_path[1024];
	double nx_start, nx_end, nx_dur;
	float nx_rg;
	int nx_req, nx_xfade, seq;    /* seq растёт при каждом переходе на следующий трек */
	/* заказ из audio_play_range: открыть в потоке; gen растёт после открытия */
	char rq_path[1024];
	double rq_start, rq_end, rq_dur;
	int rq, gen;
	int xfade_ms;
	/* команды и состояние */
	int paused, ended, volume;
	double seek_to;
	float level, spec[AUDIO_BANDS];
	/* эквалайзер: новые настройки от интерфейса (под мьютексом), применяются потоком */
	int eq_on, eq_dirty;
	float rg_pend;   /* ReplayGain для ближайшего audio_play_range */
	float eq_gain[EQ_BANDS];
	/* вывод */
	int out_rate;
#if XMB_DEVICE
	struct pcm *pcm;
	struct mixer *mixer;
	int port;
	double idle;
#else
	SDL_AudioDeviceID dev;
#endif
} a;

/* заявки интерфейса — под своим коротким мьютексом; поток забирает их в начале круга. Под a.mu поток открывает
 * файлы и декодирует, и интерфейс, ждавший его на смене трека, пропускал кадры (открытие — десятки мс) */
static struct {
	pthread_mutex_t mu;
	int play, next, pause, seek, rg;   /* есть заявка */
	char play_path[1024];
	double play_start, play_end, play_dur;
	char next_path[1024];
	double next_start, next_end, next_dur;
	float next_rg, rg_v;
	int next_on, next_xfade, pause_v;
	double seek_v;
} cmd = { .mu = PTHREAD_MUTEX_INITIALIZER };

int audio_supported(const char *path)
{
	const char *e = strrchr(path, '.');
	return e && (!strcasecmp(e, ".flac") || !strcasecmp(e, ".mp3") || !strcasecmp(e, ".wav") || !strcasecmp(e, ".m4a"));
}

/* ---------- декодер (вызывается под мьютексом) ---------- */

/* заголовок Xing/Info первого кадра: забираем оглавление для перемотки (dr_mp3 его пропускает) */
static void mp3_meta(void *user, const drmp3_metadata *m)
{
	dec_t *d = user;
	const unsigned char *p = m->pRawData;
	size_t n = m->rawDataSize, i = 8;
	if (n < 8 || (memcmp(p, "Xing", 4) && memcmp(p, "Info", 4)))
		return;
	unsigned flags = p[7];
	uint32_t bytes = 0;
	if (flags & 1)
		i += 4;
	if (flags & 2) {
		if (n < i + 4)
			return;
		bytes = (uint32_t)p[i] << 24 | (uint32_t)p[i + 1] << 16 | (uint32_t)p[i + 2] << 8 | p[i + 3];
		i += 4;
	}
	if ((flags & 4) && bytes && n >= i + 100) {
		memcpy(d->toc, p + i, 100);
		d->toc_bytes = bytes;
	}
}

static void dec_close(dec_t *d)
{
	if (d->codec == C_FLAC)
		drflac_close(d->flac);
	else if (d->codec == C_MP3)
		drmp3_uninit(&d->mp3);
	else if (d->codec == C_WAV)
		drwav_uninit(&d->wav);
	else if (d->codec == C_ALAC) {
		alacdec_close(d->alac);
		mp4_close(d->mp4);
		d->alac = NULL;
		d->mp4 = NULL;
	} else if (d->codec == C_AAC) {
		NeAACDecClose(d->aac);
		mp4_close(d->mp4);
		d->aac = NULL;
		d->mp4 = NULL;
	}
	d->codec = C_NONE;
	d->have = 0;
}

static int dec_open(dec_t *d, const char *path)
{
	const char *e = strrchr(path, '.');
	if (!e)
		return -1;
	if (!strcasecmp(e, ".flac")) {
		d->flac = drflac_open_file(path, NULL);
		if (!d->flac)
			return -1;
		d->codec = C_FLAC;
		d->rate = d->flac->sampleRate;
		d->channels = d->flac->channels;
		d->bits = d->flac->bitsPerSample;
		d->total = d->flac->totalPCMFrameCount;
	} else if (!strcasecmp(e, ".mp3")) {
		d->toc_bytes = 0;
		if (!drmp3_init_file_with_metadata(&d->mp3, path, mp3_meta, d, NULL))
			return -1;
		d->codec = C_MP3;
		d->rate = d->mp3.sampleRate;
		d->channels = d->mp3.channels;
		d->bits = 16;
		d->total = 0;   /* точная длина MP3 — только полным проходом; берём подсказку библиотеки */
	} else if (!strcasecmp(e, ".wav")) {
		if (!drwav_init_file(&d->wav, path, NULL))
			return -1;
		d->codec = C_WAV;
		d->rate = d->wav.sampleRate;
		d->channels = d->wav.channels;
		d->bits = d->wav.bitsPerSample;
		d->total = d->wav.totalPCMFrameCount;
	} else if (!strcasecmp(e, ".m4a")) {
		/* .m4a бывает и AAC, и ALAC — различает только описание дорожки */
		d->mp4 = mp4_open(path);
		if (d->mp4 && mp4_audio_codec(d->mp4) == MP4_CODEC_AAC)
			return aac_open(d);
		int cl = 0;
		const unsigned char *ck = d->mp4 && mp4_audio_codec(d->mp4) == MP4_CODEC_ALAC ? mp4_audio_config(d->mp4, &cl) : NULL;
		d->alac = ck ? alacdec_open(ck, cl) : NULL;
		if (!d->alac) {
			if (d->mp4)
				mp4_close(d->mp4);
			d->mp4 = NULL;
			return -1;
		}
		d->codec = C_ALAC;
		d->rate = alacdec_sample_rate(d->alac);
		d->channels = alacdec_channels(d->alac);
		d->bits = alacdec_bits(d->alac);
		d->total = (unsigned long long)(mp4_duration_seconds(d->mp4) * d->rate + 0.5);
		d->au = 0;
		d->pcm_n = d->pcm_pos = 0;
		d->alac_pcm = realloc(d->alac_pcm, sizeof(int32_t) * alacdec_frame_samples(d->alac) * d->channels);
		d->au_size = mp4_max_frame_size(d->mp4);
		d->au_buf = realloc(d->au_buf, d->au_size ? d->au_size : 1);
	} else {
		return -1;
	}
	d->frame = 0;
	d->have = 1;
	return 0;
}

/* AAC: AudioSpecificConfig из контейнера, выход — 16 бит, многоканальное сводится в стерео.
 * Частота и каналы — по первому раскодированному пакету: при SBR (HE-AAC) объявленная частота вдвое меньше */
static int aac_next_packet(dec_t *d);

static int aac_open(dec_t *d)
{
	int cl = 0;
	const unsigned char *ck = mp4_audio_config(d->mp4, &cl);
	d->aac = ck ? NeAACDecOpen() : NULL;
	if (!d->aac) {
		mp4_close(d->mp4);
		d->mp4 = NULL;
		return -1;
	}
	NeAACDecConfigurationPtr cfg = NeAACDecGetCurrentConfiguration(d->aac);
	cfg->outputFormat = FAAD_FMT_16BIT;
	cfg->downMatrix = 1;
	NeAACDecSetConfiguration(d->aac, cfg);
	unsigned long rate = 0;
	unsigned char ch = 0;
	if (NeAACDecInit2(d->aac, (unsigned char *)ck, (unsigned long)cl, &rate, &ch) < 0) {
		NeAACDecClose(d->aac);
		mp4_close(d->mp4);
		d->aac = NULL;
		d->mp4 = NULL;
		return -1;
	}
	d->codec = C_AAC;
	d->rate = (int)rate;
	d->channels = ch > 2 ? 2 : ch;
	d->bits = 16;
	d->au = 0;
	d->pcm_n = d->pcm_pos = 0;
	d->au_size = mp4_max_frame_size(d->mp4);
	d->au_buf = realloc(d->au_buf, d->au_size ? d->au_size : 1);
	if (!d->au_size)
		d->au_size = 1;
	if (!aac_next_packet(d)) {   /* первый пакет: настоящая частота и число каналов */
		dec_close(d);
		return -1;
	}
	d->total = (unsigned long long)(mp4_duration_seconds(d->mp4) * d->rate + 0.5);
	d->frame = 0;
	d->have = 1;
	return 0;
}

static int aac_next_packet(dec_t *d)
{
	while (d->au < mp4_audio_frame_count(d->mp4)) {
		int len = mp4_read_audio_frame(d->mp4, d->au, d->au_buf, d->au_size);
		if (len < 0) {   /* пакет больше буфера */
			d->au_size = (uint32_t)-len;
			d->au_buf = realloc(d->au_buf, d->au_size);
			continue;
		}
		d->au++;
		if (len == 0)
			return 0;
		NeAACDecFrameInfo info;
		int16_t *pcm = NeAACDecDecode(d->aac, &info, d->au_buf, (unsigned long)len);
		if (info.error || !pcm || !info.samples || !info.channels)
			continue;   /* битый пакет или задержка декодера — следующий */
		int ch = info.channels > 2 ? 2 : info.channels, frames = (int)(info.samples / info.channels);
		d->aac_pcm = realloc(d->aac_pcm, sizeof(int16_t) * frames * 2);
		for (int i = 0; i < frames; i++)
			for (int c = 0; c < 2; c++)
				d->aac_pcm[i * 2 + c] = pcm[i * info.channels + (c < ch ? c : 0)];
		if (info.samplerate)
			d->rate = (int)info.samplerate;
		d->channels = 2;
		d->pcm_n = frames;
		d->pcm_pos = 0;
		return 1;
	}
	return 0;
}

static unsigned aac_read(dec_t *d, int16_t *dst, unsigned n)
{
	unsigned got = 0;
	while (got < n) {
		if (d->pcm_pos >= d->pcm_n && !aac_next_packet(d))
			break;
		unsigned take = (unsigned)(d->pcm_n - d->pcm_pos);
		if (take > n - got)
			take = n - got;
		memcpy(dst + (size_t)got * 2, d->aac_pcm + (size_t)d->pcm_pos * 2, take * 4);
		got += take;
		d->pcm_pos += (int)take;
	}
	return got;
}

/* ALAC: пакеты по очереди, из каждого — до frame_samples кадров (32 бита, выровнены влево) */
static int alac_next_packet(dec_t *d)
{
	while (d->au < mp4_audio_frame_count(d->mp4)) {
		int len = mp4_read_audio_frame(d->mp4, d->au, d->au_buf, d->au_size);
		if (len < 0) {   /* пакет больше буфера: mp4.c возвращает нужный размер со знаком минус */
			d->au_size = (uint32_t)-len;
			d->au_buf = realloc(d->au_buf, d->au_size);
			continue;
		}
		d->au++;
		if (len == 0)
			return 0;
		d->pcm_n = alacdec_decode(d->alac, d->au_buf, len, d->alac_pcm, alacdec_frame_samples(d->alac));
		d->pcm_pos = 0;
		if (d->pcm_n > 0)
			return 1;
	}
	return 0;
}

static unsigned alac_read(dec_t *d, int16_t *dst, unsigned n, unsigned ch)
{
	unsigned got = 0;
	while (got < n) {
		if (d->pcm_pos >= d->pcm_n && !alac_next_packet(d))
			break;
		unsigned take = (unsigned)(d->pcm_n - d->pcm_pos);
		if (take > n - got)
			take = n - got;
		const int32_t *s = d->alac_pcm + (size_t)d->pcm_pos * d->channels;
		for (unsigned i = 0; i < take * ch; i++)
			dst[(size_t)got * ch + i] = (int16_t)(s[i] >> 16);
		got += take;
		d->pcm_pos += (int)take;
	}
	return got;
}

/* читает до n кадров в buf как 16 бит стерео; 0 — конец */
static unsigned dec_read(dec_t *d, int16_t *buf, unsigned n)
{
	int16_t tmp[CHUNK * 8];
	if (d->end_f) {
		if (d->frame >= d->end_f)
			return 0;
		if (d->frame + n > d->end_f)
			n = (unsigned)(d->end_f - d->frame);
	}
	unsigned ch = d->channels > 8 ? 8 : d->channels, got = 0;
	int16_t *dst = ch == 2 ? buf : tmp;
	if (d->codec == C_FLAC)
		got = (unsigned)drflac_read_pcm_frames_s16(d->flac, n, dst);
	else if (d->codec == C_MP3)
		got = (unsigned)drmp3_read_pcm_frames_s16(&d->mp3, n, dst);
	else if (d->codec == C_WAV)
		got = (unsigned)drwav_read_pcm_frames_s16(&d->wav, n, dst);
	else if (d->codec == C_ALAC)
		got = alac_read(d, dst, n, ch);
	else if (d->codec == C_AAC)   /* уже стерео */
		got = aac_read(d, dst, n);
	if (ch != 2)
		for (unsigned i = 0; i < got; i++) {
			buf[i * 2] = tmp[i * ch];
			buf[i * 2 + 1] = tmp[i * ch + (ch > 1)];
		}
	d->frame += got;
	return got;
}

/* Перемотка FLAC без перебора с начала. Двоичный поиск dr_flac на некоторых позициях больших образов без SEEKTABLE
 * не находит кадр и откатывается на перебор от начала файла (на 24/96 — 15–20 с). Тогда поиск повторяется чуть раньше
 * цели (на другой позиции он проходит), а остаток добирается обычным декодированием (~25× быстрее реального времени). */
static int flac_seek(drflac *f, unsigned long long target)
{
	static drflac_int32 scratch[4096 * 8];
	static const int back[] = { 1, 3, 7, 15, 30 };
	f->_noBruteForceSeek = DRFLAC_TRUE;
	int ok = drflac_seek_to_pcm_frame(f, target);
	for (unsigned i = 0; !ok && i < sizeof(back) / sizeof(back[0]); i++) {
		unsigned long long d = (unsigned long long)back[i] * f->sampleRate;
		if (target <= d || !drflac_seek_to_pcm_frame(f, target - d))
			continue;
		while (d > 0) {
			drflac_uint64 n = drflac_read_pcm_frames_s32(f, d < 4096 ? d : 4096, scratch);
			if (!n)
				break;
			d -= n;
		}
		ok = d == 0;
	}
	f->_noBruteForceSeek = DRFLAC_FALSE;
	return ok || drflac_seek_to_pcm_frame(f, target);   /* в крайнем случае — перебор */
}

/* MP3: dr_mp3 без таблицы перематывает декодированием всего подряд (назад — с начала файла), на минуты вперёд это
 * секунды. Дальний переход — сразу на байтовую позицию: по оглавлению Xing (VBR) или пропорционально (CBR),
 * ближайший кадр декодер находит сам. Близкий — как раньше, точно */
static void mp3_seek(dec_t *d, unsigned long long f)
{
	drmp3 *m = &d->mp3;
	unsigned long long total = m->totalPCMFrameCount != DRMP3_UINT64_MAX ? m->totalPCMFrameCount - m->delayInPCMFrames - m->paddingInPCMFrames
		: (unsigned long long)(d->dur_hint * d->rate);
	struct stat sb;
	FILE *fp = m->onRead == drmp3__on_read_stdio ? m->pUserData : NULL;
	int near = f < (unsigned long long)d->rate * 3 || (f >= d->frame && f - d->frame < (unsigned long long)d->rate * 3);
	if (near || !total || !fp || fstat(fileno(fp), &sb) || (unsigned long long)sb.st_size <= m->streamStartOffset) {
		drmp3_seek_to_pcm_frame(m, f);
		return;
	}
	double frac = (double)f / total, byte;
	frac = frac > 0.999 ? 0.999 : frac;
	if (d->toc_bytes) {
		double pct = frac * 100;
		int i = (int)pct;
		double a = d->toc[i], b = i < 99 ? d->toc[i + 1] : 256;
		byte = (a + (b - a) * (pct - i)) / 256 * d->toc_bytes;
	} else {
		byte = frac * (double)(sb.st_size - m->streamStartOffset);
	}
	if (!drmp3__on_seek_64(m, m->streamStartOffset + (drmp3_uint64)byte, DRMP3_SEEK_SET)) {
		drmp3_seek_to_pcm_frame(m, f);
		return;
	}
	drmp3_reset(m);
	m->currentPCMFrame = f + m->delayInPCMFrames;   /* счёт кадров dr_mp3 — с задержкой кодера, как в его перемотке */
}

/* sec — от начала трека (у трека из CUE — от его INDEX 01) */
static void dec_seek(dec_t *d, double sec)
{
	unsigned long long f = d->start_f + (unsigned long long)(sec * d->rate);
	if (d->codec == C_FLAC)
		flac_seek(d->flac, f);
	else if (d->codec == C_MP3)
		mp3_seek(d, f);
	else if (d->codec == C_WAV)
		drwav_seek_to_pcm_frame(&d->wav, f);
	else if (d->codec == C_AAC) {
		double t = (double)f / d->rate;
		d->au = mp4_frame_at_time(d->mp4, t);
		NeAACDecPostSeekReset(d->aac, (long)d->au);
		d->pcm_n = d->pcm_pos = 0;
		long skip = (long)((t - mp4_frame_time(d->mp4, d->au)) * d->rate);
		if (aac_next_packet(d) && skip > 0)
			d->pcm_pos = skip < d->pcm_n ? (int)skip : d->pcm_n;
	} else if (d->codec == C_ALAC) {   /* пакет, в который попадает время, и пропуск до нужного кадра в нём */
		double t = (double)f / d->rate;
		d->au = mp4_frame_at_time(d->mp4, t);
		d->pcm_n = d->pcm_pos = 0;
		long skip = (long)((t - mp4_frame_time(d->mp4, d->au)) * d->rate);
		if (alac_next_packet(d) && skip > 0)
			d->pcm_pos = skip < d->pcm_n ? (int)skip : d->pcm_n;
	}
	d->frame = f;
}

/* кадров до конца трека; длина MP3 неизвестна точно — по подсказке библиотеки */
static unsigned long long dec_left(const dec_t *d)
{
	unsigned long long end = d->end_f ? d->end_f : d->total ? d->total : (unsigned long long)(d->dur_hint * d->rate);
	if (!end)
		return ~0ULL;   /* длина неизвестна — конец узнаем, когда кончится файл */
	return end > d->frame ? end - d->frame : 0;
}

/* открыть трек (у трека из CUE — с перемоткой к его началу) */
static int dec_start(dec_t *d, const char *path, double start, double end, double dur_hint, float rg)
{
	dec_close(d);
	int r = dec_open(d, path);
	snprintf(d->path, sizeof(d->path), "%s", r ? "" : path);
	d->start_f = r ? 0 : (unsigned long long)(start * d->rate + 0.5);
	d->end_f = !r && end > start ? (unsigned long long)(end * d->rate + 0.5) : 0;
	d->dur_hint = dur_hint;
	d->rg = rg;
	if (!r && d->start_f)
		dec_seek(d, 0);
	return r;
}

/* ---------- вывод ---------- */

#if XMB_DEVICE
static void mixer_set(const char *name, int v)
{
	if (!a.mixer)
		a.mixer = mixer_open(0);
	struct mixer_ctl *c = a.mixer ? mixer_get_ctl_by_name(a.mixer, name) : NULL;
	if (c)
		mixer_ctl_set_value(c, 0, v);
}

/* кривые громкости стока (/usr/resource/ot_devices.json, VOLUMES «HW»: HDB, MDB, LDB) — ослабление ES9219
 * шагом 0.5 дБ для 0…100%; внизу шаги крупнее, вверху — по 0.5 дБ */
static const uint8_t vol_curve[3][101] = {
	{ 255, 176, 166, 156, 146, 142, 138, 134, 130, 126, 124, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104,
	  102, 100, 98, 96, 94, 92, 90, 88, 86, 84, 82, 80, 78, 76, 74, 72, 70, 68, 66, 64,
	  62, 60, 58, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40,
	  39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20,
	  19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0 },
	{ 255, 188, 178, 168, 158, 154, 150, 146, 142, 138, 136, 134, 132, 130, 128, 126, 124, 122, 120, 118, 116,
	  114, 112, 110, 108, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88, 86, 84, 82, 80, 78, 76,
	  74, 72, 70, 68, 67, 66, 65, 64, 63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52,
	  51, 50, 49, 48, 47, 46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,
	  31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12 },
	{ 255, 200, 190, 180, 170, 166, 162, 158, 154, 150, 148, 146, 144, 142, 140, 138, 136, 134, 132, 130, 128,
	  126, 124, 122, 120, 118, 116, 114, 112, 110, 108, 106, 104, 102, 100, 98, 96, 94, 92, 90, 88,
	  86, 84, 82, 80, 79, 78, 77, 76, 75, 74, 73, 72, 71, 70, 69, 68, 67, 66, 65, 64,
	  63, 62, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47, 46, 45, 44,
	  43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32, 31, 30, 29, 28, 27, 26, 25, 24 },
};
static int gain_mode;

static void apply_volume(void)
{
	int att = vol_curve[gain_mode][a.volume < 0 ? 0 : a.volume > 100 ? 100 : a.volume];
	mixer_set("Left Playback Volume", att);
	mixer_set("Right Playback Volume", att);
}

static void out_close(void)
{
	if (a.pcm)
		pcm_close(a.pcm);
	a.pcm = NULL;
	a.out_rate = 0;
}

static int out_open(int rate)
{
	if (a.pcm && a.out_rate == rate)
		return 0;
	out_close();
	if (a.port != PORT_PHONES) {
		mixer_set("Output Port Switch", PORT_PHONES);   /* до открытия PCM: иначе ядро падает */
		a.port = PORT_PHONES;
		apply_volume();
	}
	struct pcm_config cfg = {
		.channels = 2, .rate = rate, .period_size = CHUNK, .period_count = 4, .format = PCM_FORMAT_S16_LE,
	};
	a.pcm = pcm_open(0, 0, PCM_OUT, &cfg);
	if (!a.pcm || !pcm_is_ready(a.pcm)) {
		fprintf(stderr, "audio: pcm_open: %s\n", a.pcm ? pcm_get_error(a.pcm) : "?");
		out_close();
		return -1;
	}
	a.out_rate = rate;
	return 0;
}

static void out_write(const int16_t *buf, unsigned frames)
{
	if (pcm_writei(a.pcm, buf, frames) < 0)
		pcm_prepare(a.pcm);
}

static void out_power_off(void)
{
	out_close();
	if (a.port != PORT_OFF) {
		mixer_set("Output Port Switch", PORT_OFF);
		a.port = PORT_OFF;
	}
}
#else
static int out_open(int rate)
{
	if (a.dev && a.out_rate == rate)
		return 0;
	if (a.dev)
		SDL_CloseAudioDevice(a.dev);
	SDL_AudioSpec want = { .freq = rate, .format = AUDIO_S16SYS, .channels = 2, .samples = CHUNK };
	a.dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
	if (!a.dev) {
		fprintf(stderr, "audio: SDL: %s\n", SDL_GetError());
		return -1;
	}
	SDL_PauseAudioDevice(a.dev, 0);
	a.out_rate = rate;
	return 0;
}

static void out_write(int16_t *buf, unsigned frames)
{
	for (unsigned i = 0; i < frames * 2; i++)
		buf[i] = (int16_t)(buf[i] * a.volume / 100);
	/* очередь SDL держим около 4 периодов: этим поток и тормозится, как блокирующей записью */
	while (SDL_GetQueuedAudioSize(a.dev) > CHUNK * 4 * 4)
		SDL_Delay(5);
	SDL_QueueAudio(a.dev, buf, frames * 4);
}

static void apply_volume(void) {}
static void out_power_off(void) {}
#endif

/* ---------- спектр для визуализатора ---------- */

#define FFT_N 1024

/* БПФ по последнему куску звука (моно, окно Ханна) → AUDIO_BANDS логарифмических полос в дБ */
static void spectrum(const int16_t *buf, unsigned frames, int rate)
{
	static float win[FFT_N], cs[FFT_N / 2], sn[FFT_N / 2], re[FFT_N], im[FFT_N];
	static int ready, lo[AUDIO_BANDS], hi[AUDIO_BANDS], band_rate;
	if (!ready) {
		for (int i = 0; i < FFT_N; i++)
			win[i] = 0.5f - 0.5f * cosf(2 * (float)M_PI * i / (FFT_N - 1));
		for (int i = 0; i < FFT_N / 2; i++) {
			cs[i] = cosf(2 * (float)M_PI * i / FFT_N);
			sn[i] = -sinf(2 * (float)M_PI * i / FFT_N);
		}
		ready = 1;
	}
	if (band_rate != rate) {   /* границы полос в бинах — под частоту файла */
		double fmax = rate / 2.0 < 16000 ? rate / 2.0 : 16000;
		for (int b = 0; b < AUDIO_BANDS; b++) {
			double f0 = 40 * pow(fmax / 40, (double)b / AUDIO_BANDS), f1 = 40 * pow(fmax / 40, (double)(b + 1) / AUDIO_BANDS);
			lo[b] = (int)(f0 * FFT_N / rate);
			hi[b] = (int)(f1 * FFT_N / rate);
			if (lo[b] < 1)
				lo[b] = 1;
			if (hi[b] <= lo[b])
				hi[b] = lo[b] + 1;
			if (hi[b] > FFT_N / 2)
				hi[b] = FFT_N / 2;
		}
		band_rate = rate;
	}
	unsigned n = frames < FFT_N ? frames : FFT_N;
	for (unsigned i = 0; i < FFT_N; i++) {
		float v = i < n ? (buf[i * 2] + buf[i * 2 + 1]) * 0.5f : 0;
		re[i] = v * win[i];
		im[i] = 0;
	}
	/* итеративное БПФ по основанию 2 */
	for (unsigned i = 1, j = 0; i < FFT_N; i++) {
		unsigned bit = FFT_N >> 1;
		for (; j & bit; bit >>= 1)
			j ^= bit;
		j ^= bit;
		if (i < j) {
			float t = re[i]; re[i] = re[j]; re[j] = t;
			t = im[i]; im[i] = im[j]; im[j] = t;
		}
	}
	for (unsigned len = 2; len <= FFT_N; len <<= 1) {
		unsigned step = FFT_N / len;
		for (unsigned i = 0; i < FFT_N; i += len)
			for (unsigned k = 0; k < len / 2; k++) {
				float wr = cs[k * step], wi = sn[k * step];
				unsigned p = i + k, q = p + len / 2;
				float xr = re[q] * wr - im[q] * wi, xi = re[q] * wi + im[q] * wr;
				re[q] = re[p] - xr;
				im[q] = im[p] - xi;
				re[p] += xr;
				im[p] += xi;
			}
	}
	/* 0 дБ — синус полной шкалы; мощность полосы — сумма по её бинам */
	const float ref = (FFT_N / 4.0f) * 32768.0f;
	for (int b = 0; b < AUDIO_BANDS; b++) {
		float pw = 0;
		for (int k = lo[b]; k < hi[b]; k++)
			pw += re[k] * re[k] + im[k] * im[k];
		float db = 10 * log10f(pw / (ref * ref) + 1e-12f);
		float v = (db + 60) / 60;
		a.spec[b] = v < 0 ? 0 : v > 1 ? 1 : v;
	}
}

/* ---------- эквалайзер ---------- */

const int eq_freq[EQ_BANDS] = { 31, 62, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

/* пиковые фильтры RBJ (Q ≈ 1.4 — октавные полосы), транспонированная прямая форма II, float.
 * Перед ними — ослабление на самый большой подъём, чтобы усиленные полосы не перегружали 16 бит */
static struct {
	int on, n, rate;
	float pre;
	struct { float b0, b1, b2, a1, a2, z[2][2]; } f[EQ_BANDS];
} eq;

static void eq_setup(int on, const float *gain, int rate)
{
	float top = 0;
	eq.n = 0;
	eq.rate = rate;
	for (int i = 0; i < EQ_BANDS && on; i++) {
		if (gain[i] > top)
			top = gain[i];
		if (fabsf(gain[i]) < 0.05f || eq_freq[i] > rate * 0.45)
			continue;
		double A = pow(10, gain[i] / 40.0), w = 2 * M_PI * eq_freq[i] / rate, al = sin(w) / (2 * 1.41);
		double a0 = 1 + al / A;
		eq.f[eq.n].b0 = (float)((1 + al * A) / a0);
		eq.f[eq.n].b1 = (float)(-2 * cos(w) / a0);
		eq.f[eq.n].b2 = (float)((1 - al * A) / a0);
		eq.f[eq.n].a1 = eq.f[eq.n].b1;
		eq.f[eq.n].a2 = (float)((1 - al / A) / a0);
		memset(eq.f[eq.n].z, 0, sizeof(eq.f[eq.n].z));
		eq.n++;
	}
	eq.pre = powf(10, -top / 20);
	eq.on = on && eq.n > 0;
}

static void eq_process(int16_t *buf, unsigned frames)
{
	for (unsigned i = 0; i < frames; i++)
		for (int c = 0; c < 2; c++) {
			float x = buf[i * 2 + c] * eq.pre + 1e-18f;   /* добавка — против денормалов на хвостах */
			for (int k = 0; k < eq.n; k++) {
				float y = eq.f[k].b0 * x + eq.f[k].z[c][0];
				eq.f[k].z[c][0] = eq.f[k].b1 * x - eq.f[k].a1 * y + eq.f[k].z[c][1];
				eq.f[k].z[c][1] = eq.f[k].b2 * x - eq.f[k].a2 * y;
				x = y;
			}
			int v = (int)lrintf(x);
			buf[i * 2 + c] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
		}
}

void audio_set_eq(int on, const float gain[EQ_BANDS])
{
	pthread_mutex_lock(&a.mu);
	a.eq_on = on;
	memcpy(a.eq_gain, gain, sizeof(a.eq_gain));
	a.eq_dirty = 1;
	pthread_mutex_unlock(&a.mu);
}

/* ---------- поток ---------- */

static void apply_rg(int16_t *buf, unsigned n, float k)
{
	if (k == 1.0f || k <= 0)
		return;
	for (unsigned i = 0; i < n * 2; i++) {
		int v = (int)lrintf(buf[i] * k);
		buf[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
	}
}

/* Следующий трек открывается за PREP_S секунд до конца текущего (открытие и перемотка в образе — до долей секунды)
 * или за время кроссфейда. Конец текущего: следующий подхватывается сразу, без тишины (gapless). Кроссфейд —
 * если частоты совпадают и интерфейс его разрешил: последние xfade мс текущий затухает, следующий нарастает */
#define PREP_S 3

static volatile int spec_on = 1;   /* считать спектр: только пока экран включён */
static void open_request(void);

/* заявки интерфейса — в том порядке, в каком их отменяют друг друга (под a.mu) */
static void take_cmds(void)
{
	static typeof(cmd) c;
	pthread_mutex_lock(&cmd.mu);
	int any = cmd.play || cmd.next || cmd.pause || cmd.seek || cmd.rg;
	if (any) {
		memcpy(&c, &cmd, sizeof(c));
		cmd.play = cmd.next = cmd.pause = cmd.seek = cmd.rg = 0;
	}
	pthread_mutex_unlock(&cmd.mu);
	if (!any)
		return;
	if (c.rg)
		a.rg_pend = a.d[a.cur].rg = c.rg_v;
	if (c.play) {
		dec_close(&a.d[!a.cur]);   /* заказанный следующий больше не следующий */
		a.nx_req = 0;
		snprintf(a.rq_path, sizeof(a.rq_path), "%s", c.play_path);
		a.rq_start = c.play_start;
		a.rq_end = c.play_end;
		a.rq_dur = c.play_dur;
		a.rq = 1;
		a.paused = 0;
		a.ended = 0;
		a.seek_to = -1;
	}
	if (c.next) {
		dec_close(&a.d[!a.cur]);
		a.nx_req = c.next_on;
		snprintf(a.nx_path, sizeof(a.nx_path), "%s", c.next_path);
		a.nx_start = c.next_start;
		a.nx_end = c.next_end;
		a.nx_dur = c.next_dur;
		a.nx_xfade = c.next_xfade;
		a.nx_rg = c.next_rg;
	}
	if (c.pause)
		a.paused = c.pause_v;
	if (c.seek) {
		a.seek_to = c.seek_v;
		a.ended = 0;
	}
}

static void *audio_thread(void *arg)
{
	(void)arg;
	static int16_t buf[CHUNK * 2], nb[CHUNK * 2];
	for (;;) {
		pthread_mutex_lock(&a.mu);
		take_cmds();
		if (a.rq)
			open_request();
		dec_t *c = &a.d[a.cur], *n = &a.d[!a.cur];
		if (a.seek_to >= 0 && c->have) {
			dec_seek(c, a.seek_to);
			a.seek_to = -1;
			dec_close(n);   /* после перемотки следующий откроется заново, к новому концу */
		}
		int play = c->have && !a.paused && !a.ended, rate = c->rate;
		if (a.eq_dirty || (play && eq.rate != rate)) {   /* новые полосы или частота файла */
			eq_setup(a.eq_on, a.eq_gain, rate ? rate : 44100);
			a.eq_dirty = 0;
		}
		unsigned long long xf = (unsigned long long)a.xfade_ms * (unsigned)(rate ? rate : 44100) / 1000;
		if (!a.nx_xfade)
			xf = 0;
		/* заранее открыть следующий */
		if (play && a.nx_req && !n->have && dec_left(c) <= (unsigned long long)PREP_S * rate + xf) {
			if (dec_start(n, a.nx_path, a.nx_start, a.nx_end, a.nx_dur, a.nx_rg))
				a.nx_req = 0;   /* не открылся — дальше обычный конец */
		}
		unsigned got = play ? dec_read(c, buf, CHUNK) : 0;
		if (got)
			apply_rg(buf, got, c->rg);
		/* кроссфейд: хвост текущего смешивается с началом следующего (только при одной частоте) */
		int mixing = got && xf && n->have && n->rate == c->rate && dec_left(c) + got <= xf;
		if (mixing) {
			unsigned m = dec_read(n, nb, got);
			apply_rg(nb, m, n->rg);
			unsigned long long left = dec_left(c);
			for (unsigned i = 0; i < got; i++) {
				float w = (float)(left + got - i) / (float)xf;   /* доля текущего: от 1 к 0 */
				w = w > 1 ? 1 : w;
				for (int ch = 0; ch < 2; ch++) {
					float v = buf[i * 2 + ch] * w + (i < m ? nb[i * 2 + ch] : 0) * (1 - w);
					buf[i * 2 + ch] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
				}
			}
		}
		if (play && !got) {
			if (n->have) {   /* следующий — сразу, без паузы */
				dec_close(c);
				a.cur = !a.cur;
				a.nx_req = 0;
				a.seq++;
				c = &a.d[a.cur];
				rate = c->rate;
				got = dec_read(c, buf, CHUNK);
				apply_rg(buf, got, c->rg);
			}
			if (!got) {
				a.ended = 1;
				play = 0;
			}
		}
		pthread_mutex_unlock(&a.mu);

		if (got) {
			double sum = 0;
			for (unsigned i = 0; i < got * 2; i += 2)
				sum += (double)buf[i] * buf[i];
			float rms = (float)(sqrt(sum / got) / 32768.0);
			a.level = rms > 1 ? 1 : rms;
			if (eq.on)
				eq_process(buf, got);
			if (spec_on)   /* экран погашен — спектр никто не видит */
				spectrum(buf, got, rate);
#if XMB_DEVICE
			a.idle = 0;
#endif
			if (out_open(rate) == 0)
				out_write(buf, got);
			else
				usleep(20000);
			continue;
		}
		a.level = 0;
		memset(a.spec, 0, sizeof(a.spec));
#if XMB_DEVICE
		/* пауза: поток держим тишиной (иначе на продолжении теряется секунда), потом гасим тракт */
		if (a.pcm && a.idle < IDLE_POWER_OFF) {
			memset(buf, 0, sizeof(buf));
			out_write(buf, CHUNK);
			a.idle += (double)CHUNK / a.out_rate;
			continue;
		}
		if (a.pcm)
			out_power_off();
#endif
		usleep(20000);
	}
	return NULL;
}

/* ---------- API ---------- */

int audio_init(void)
{
	pthread_mutex_init(&a.mu, NULL);
	a.volume = 42;
	a.seek_to = -1;
	a.d[0].rg = a.d[1].rg = a.rg_pend = a.nx_rg = 1;
#if XMB_DEVICE
	a.port = -1;
#else
	SDL_InitSubSystem(SDL_INIT_AUDIO);
#endif
	apply_volume();
	return pthread_create(&a.thread, NULL, audio_thread, NULL);
}

int audio_play_range(const char *path, double start, double end, double dur_hint)
{
	pthread_mutex_lock(&cmd.mu);
	cmd.play = 1;
	snprintf(cmd.play_path, sizeof(cmd.play_path), "%s", path);
	cmd.play_start = start;
	cmd.play_end = end;
	cmd.play_dur = dur_hint;
	cmd.next = cmd.pause = cmd.seek = 0;   /* новый трек отменяет прежние заявки на следующий, паузу и перемотку */
	pthread_mutex_unlock(&cmd.mu);
	return 0;
}

/* открыть заказанный трек (в потоке, под мьютексом) */
static void open_request(void)
{
	dec_t *c = &a.d[a.cur];
	int r = 0;
	a.rq = 0;
	/* следующий трек того же образа сразу за текущим — без переоткрытия и перемотки, без паузы */
	int cont = c->have && !strcmp(c->path, a.rq_path) && c->rate &&
		   (unsigned long long)(a.rq_start * c->rate + 0.5) == c->frame;
	if (cont) {
		c->start_f = c->frame;
		c->end_f = a.rq_end > a.rq_start ? (unsigned long long)(a.rq_end * c->rate + 0.5) : 0;
		c->dur_hint = a.rq_dur;
		c->rg = a.rg_pend;
	} else {
		r = dec_start(c, a.rq_path, a.rq_start, a.rq_end, a.rq_dur, a.rg_pend);
	}
	a.ended = r != 0;
	a.gen++;
	if (r)
		fprintf(stderr, "audio: не открылся %s\n", a.rq_path);
}

static float rg_mul(float db, float peak)
{
	float k = powf(10, db / 20);
	if (peak > 0 && k * peak > 1)
		k = 1 / peak;
	return k;
}

void audio_set_next(const char *path, double start, double end, double dur_hint, float rg_db, float rg_peak, int xfade)
{
	pthread_mutex_lock(&cmd.mu);
	cmd.next = 1;
	cmd.next_on = path != NULL;
	if (path) {
		snprintf(cmd.next_path, sizeof(cmd.next_path), "%s", path);
		cmd.next_start = start;
		cmd.next_end = end;
		cmd.next_dur = dur_hint;
		cmd.next_xfade = xfade;
		cmd.next_rg = rg_mul(rg_db, rg_peak);
	}
	pthread_mutex_unlock(&cmd.mu);
}

void audio_set_spectrum(int on)
{
	spec_on = on;
}

void audio_set_crossfade(int ms)
{
	a.xfade_ms = ms < 0 ? 0 : ms;
}

int audio_play(const char *path, double dur_hint)
{
	return audio_play_range(path, 0, 0, dur_hint);
}

void audio_release(void)
{
	pthread_mutex_lock(&cmd.mu);
	cmd.play = cmd.next = cmd.pause = cmd.seek = 0;
	pthread_mutex_unlock(&cmd.mu);
	pthread_mutex_lock(&a.mu);
	dec_close(&a.d[0]);
	dec_close(&a.d[1]);
	a.nx_req = 0;
	a.paused = 1;
	pthread_mutex_unlock(&a.mu);
}

void audio_power_down(void)
{
#if XMB_DEVICE
	a.idle = IDLE_POWER_OFF + 1;   /* поток сам закроет PCM и выключит тракт на ближайшем круге */
	for (int i = 0; i < 100 && a.pcm; i++)
		usleep(10000);
#endif
}

void audio_pause(int pause)
{
	pthread_mutex_lock(&cmd.mu);
	cmd.pause = 1;
	cmd.pause_v = pause;
	pthread_mutex_unlock(&cmd.mu);
}

void audio_seek(double sec)
{
	pthread_mutex_lock(&cmd.mu);
	cmd.seek = 1;
	cmd.seek_v = sec < 0 ? 0 : sec;
	pthread_mutex_unlock(&cmd.mu);
}

/* ReplayGain играющего трека и ближайшего audio_play_range */
void audio_set_replaygain(float db, float peak)
{
	pthread_mutex_lock(&cmd.mu);
	cmd.rg = 1;
	cmd.rg_v = rg_mul(db, peak);
	pthread_mutex_unlock(&cmd.mu);
}

void audio_set_gain(int gain)
{
#if XMB_DEVICE
	gain_mode = gain < 0 ? 0 : gain > 2 ? 2 : gain;
#endif
	apply_volume();
}

void audio_set_volume(int percent)
{
	a.volume = percent < 0 ? 0 : percent > 100 ? 100 : percent;
	apply_volume();
}

void audio_get_status(audio_status_t *st)
{
	static audio_status_t last;
	if (pthread_mutex_trylock(&a.mu)) {   /* поток занят (перемотка, открытие файла) — прошлый статус */
		*st = last;
		return;
	}
	const dec_t *c = &a.d[a.cur];
	st->playing = c->have && !a.paused && !a.ended;
	st->ended = a.ended && c->have;
	st->seq = a.seq;
	st->gen = a.gen;
	st->rate = c->rate;
	st->bits = c->bits;
	st->channels = c->channels;
	st->pos = a.seek_to >= 0 ? a.seek_to : c->rate && c->frame > c->start_f ? (double)(c->frame - c->start_f) / c->rate : 0;
	if (c->end_f && c->rate)
		st->dur = (double)(c->end_f - c->start_f) / c->rate;
	else
		st->dur = c->total && c->rate ? (double)(c->total - c->start_f) / c->rate : c->dur_hint;
	snprintf(st->codec, sizeof(st->codec), "%s", c->codec == C_FLAC ? "FLAC" : c->codec == C_MP3 ? "MP3" : c->codec == C_WAV ? "WAV" : c->codec == C_ALAC ? "ALAC" : c->codec == C_AAC ? "AAC" : "");
	st->level = a.level;
	memcpy(st->spec, a.spec, sizeof(st->spec));
	pthread_mutex_lock(&cmd.mu);
	int pend = cmd.play;   /* заявка ещё не забрана потоком */
	double pend_dur = cmd.play_dur;
	pthread_mutex_unlock(&cmd.mu);
	if (a.rq || pend) {   /* трек заказан, поток ещё не открыл: не показывать прошлый */
		st->playing = 1;
		st->ended = 0;
		st->pos = 0;
		st->dur = pend ? pend_dur : a.rq_dur;
	}
	last = *st;
	pthread_mutex_unlock(&a.mu);
}
