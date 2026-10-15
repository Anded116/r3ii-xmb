TC ?= /root/toolchains/mipsel-unknown-linux-musl
CC := $(TC)/bin/mipsel-unknown-linux-musl-gcc
CFLAGS := -O2 -march=mips32r2 -mhard-float -Wall
LDFLAGS := -static -lm

fbtest: fbtest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

host: fbtest.c
	cc -O2 -Wall -fsyntax-only $<

clean:
	rm -f fbtest
