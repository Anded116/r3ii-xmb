TC ?= /root/toolchains/mipsel-unknown-linux-musl
CC := $(TC)/bin/mipsel-unknown-linux-musl-gcc
CFLAGS := -O2 -march=mips32r2 -mhard-float -Wall
LDFLAGS := -static -lm

touchtest: touchtest.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

host: touchtest.c
	cc -O2 -Wall -fsyntax-only $<

clean:
	rm -f touchtest
