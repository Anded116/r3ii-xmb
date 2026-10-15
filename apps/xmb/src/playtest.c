/* playtest <файл> [громкость] [следующий] —проверка аудиодвижка без интерфейса: играет файл, печатает статус */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "audio.h"

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: playtest <file> [volume%%] [next-file]\n");
		return 1;
	}
	audio_init();
	audio_set_volume(argc > 2 ? atoi(argv[2]) : 30);
	if (getenv("EQ")) {   /* EQ=1 — все десять полос на +3 дБ: замер нагрузки эквалайзера */
		float g[EQ_BANDS];
		for (int i = 0; i < EQ_BANDS; i++)
			g[i] = 3;
		audio_set_eq(1, g);
	}
	/* DUR=секунды — длительность первого файла, как её даёт библиотека (у MP3 без неё длина неизвестна) */
	if (audio_play(argv[1], getenv("DUR") ? atof(getenv("DUR")) : 0))
		return 1;
	audio_status_t st;
	const char *next = argc > 3 ? argv[3] : NULL;
	if (next) {   /* переход к следующему: за 8 с до конца первого, XF=мс — кроссфейд */
		audio_set_crossfade(getenv("XF") ? atoi(getenv("XF")) : 0);
		audio_set_next(next, 0, 0, 0, 0, 0, 1);
		do {
			usleep(100000);
			audio_get_status(&st);
		} while (st.dur <= 0);
		audio_seek(st.dur - 8);
	}
	do {
		usleep(next ? 500000 : 1000000);
		audio_get_status(&st);
		printf("%s %d Hz %d bit %dch  %.1f / %.1f s  level %.2f  seq %d\n", st.codec, st.rate, st.bits, st.channels,
		       st.pos, st.dur, st.level, st.seq);
		fflush(stdout);
	} while (!st.ended && !(next && st.seq && st.pos > 6));
	return 0;
}
