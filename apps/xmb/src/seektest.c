/* seektest <файл.flac> <секунды>... — время открытия FLAC и перемотки dr_flac (без звука) */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

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

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	if (argc < 3)
		return 1;
	double t = now();
	drflac *f = drflac_open_file(argv[1], NULL);
	if (!f) {
		printf("open failed\n");
		return 1;
	}
	printf("open %.3f s: %u Hz %u bit %u ch, %llu frames, seekpoints %u\n", now() - t, f->sampleRate,
	       f->bitsPerSample, f->channels, (unsigned long long)f->totalPCMFrameCount, f->seekpointCount);
	for (int i = 2; i < argc; i++) {
		t = now();
		int ok = flac_seek(f, (drflac_uint64)(atof(argv[i]) * f->sampleRate));
		double ts = now() - t;
		static drflac_int16 buf[4096 * 8];
		t = now();
		drflac_read_pcm_frames_s16(f, 4096, buf);
		printf("seek %s s: %s %.3f s, first read %.3f s\n", argv[i], ok ? "ok" : "FAIL", ts, now() - t);
	}
	/* скорость декодирования: 20 с звука */
	t = now();
	static drflac_int16 big[8192 * 2];
	drflac_uint64 want = (drflac_uint64)f->sampleRate * 20, got = 0;
	while (got < want) {
		drflac_uint64 n = drflac_read_pcm_frames_s16(f, 8192, big);
		if (!n)
			break;
		got += n;
	}
	double td = now() - t;
	printf("decode %.1f s of audio in %.2f s (x%.1f realtime)\n", (double)got / f->sampleRate, td, got / (double)f->sampleRate / td);
	drflac_close(f);
	return 0;
}
