TC := /root/toolchains/mipsel-unknown-linux-musl
CC := $(TC)/bin/mipsel-unknown-linux-musl-gcc
inject: inject.c
	$(CC) -O2 -march=mips32r2 -Wall -static -s -o $@ $<

fbprobe: fbprobe.c
	$(CC) -O2 -march=mips32r2 -Wall -static -s -o $@ $<

fbshow: fbshow.c
	$(CC) -O2 -march=mips32r2 -Wall -static -s -o $@ $<
