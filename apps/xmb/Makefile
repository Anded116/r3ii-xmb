# make host   — окно SDL в WSL (WSLg)
# make device — статический бинарник для HiBy R3II 2025
TC := /root/toolchains/mipsel-unknown-linux-musl
LVGL_SRC := $(shell find lvgl/src -name '*.c')
SONIX := third_party/sonix/mp4.c third_party/sonix/alacdec.c third_party/sonix/alac/alac.c
FAAD2 := $(wildcard third_party/faad2/libfaad/*.c)
# FAAD2 — со своими путями и определениями: его libfaad/mp4.h не должен перекрывать third_party/sonix/mp4.h
FAAD_FLAGS := -Ithird_party/faad2/include -Ithird_party/faad2/libfaad -DHAVE_INTTYPES_H=1 -DHAVE_MEMCPY=1 \
	-DHAVE_STRING_H=1 -DHAVE_STRINGS_H=1 -DHAVE_SYS_STAT_H=1 -DHAVE_SYS_TYPES_H=1 -DHAVE_LRINTF=1 -DAPPLY_DRC \
	-DPACKAGE_VERSION=\"2.11.2\" -w
WAVES := $(addprefix src/waves/wave_,ps3.c ribbon.c particles.c spectrum.c silk.c aurora.c dust.c pulse.c ripple.c psp.c)
APP_SRC := src/xmb.c src/i18n.c src/filesave.c src/xmb_bg.c $(WAVES) src/xmb_player.c src/audio.c src/bt.c src/wifi.c src/spawn.c src/podcast.c src/lyrics.c src/library.c src/playlist.c src/cover.c third_party/tjpgd/tjpgd.c $(SONIX) $(FAAD2)
TINYALSA := $(addprefix third_party/tinyalsa/src/,pcm.c pcm_hw.c mixer.c mixer_hw.c limits.c snd_card_plugin.c)
COMMON := -Ilvgl/include -Ilvgl -Isrc -Isrc/waves -Ithird_party/dr_libs -Ithird_party/tjpgd -Ithird_party/stb -Ithird_party/sonix -Ithird_party/faad2/include -DLV_CONF_INCLUDE_SIMPLE -Wall -Wno-unused-parameter

HOST_CC := cc
HOST_CFLAGS := -O2 -g $(COMMON) -DXMB_HOST=1 -DXMB_DEVICE=0 $(shell pkg-config --cflags sdl2)
HOST_LIBS := $(shell pkg-config --libs sdl2) -lm -lpthread

DEV_CC := $(TC)/bin/mipsel-unknown-linux-musl-gcc
DEV_CFLAGS := -O2 -march=mips32r2 -mhard-float $(COMMON) -Ithird_party/tinyalsa/include -DXMB_HOST=0 -DXMB_DEVICE=1
DEV_LIBS := -static -lm -lpthread

HOST_OBJ := $(patsubst %.c,build/host/%.o,$(LVGL_SRC) $(APP_SRC) src/main_host.c)
DEV_OBJ := $(patsubst %.c,build/dev/%.o,$(LVGL_SRC) $(APP_SRC) $(TINYALSA) src/main_device.c)
PLAYTEST_OBJ := $(patsubst %.c,build/dev/%.o,src/audio.c src/spawn.c $(SONIX) $(FAAD2) $(TINYALSA) src/playtest.c)
LIBTEST_OBJ := $(patsubst %.c,build/dev/%.o,src/library.c third_party/sonix/mp4.c src/libtest.c)
SEEKTEST_OBJ := $(patsubst %.c,build/dev/%.o,src/seektest.c)
# xmbget — скачивание по HTTPS (mbedTLS) для подкастов и текстов: отдельная программа, XMB её запускает
MBEDTLS := $(wildcard third_party/mbedtls/library/*.c)
XMBGET_OBJ := $(patsubst %.c,build/dev/%.o,$(MBEDTLS) src/xmbget.c)

host: build/xmb_host
device: build/xmb build/xmbget

build/xmb_host: $(HOST_OBJ)
	$(HOST_CC) -o $@ $^ $(HOST_LIBS)

build/playtest: $(PLAYTEST_OBJ)
	$(DEV_CC) -o $@ $^ $(DEV_LIBS) -s

playtest: build/playtest

build/libtest: $(LIBTEST_OBJ)
	$(DEV_CC) -o $@ $^ $(DEV_LIBS) -s

libtest: build/libtest

build/seektest: $(SEEKTEST_OBJ)
	$(DEV_CC) -o $@ $^ $(DEV_LIBS) -s

seektest: build/seektest

build/xmb: $(DEV_OBJ)
	$(DEV_CC) -o $@ $^ $(DEV_LIBS)
	$(TC)/bin/mipsel-unknown-linux-musl-strip -o $@.stripped $@

build/host/third_party/faad2/%.o build/dev/third_party/faad2/%.o: XFLAGS := $(FAAD_FLAGS)
# /dev/random у плеера не наполняется (энтропии ~30 бит, getrandom под musl mbedTLS не берёт) — читать urandom
build/dev/third_party/mbedtls/%.o: XFLAGS := -Ithird_party/mbedtls/include -w -DMBEDTLS_PLATFORM_DEV_RANDOM=\"/dev/urandom\"
build/dev/src/xmbget.o: XFLAGS := -Ithird_party/mbedtls/include

build/xmbget: $(XMBGET_OBJ)
	$(DEV_CC) -o $@ $^ -static -s

build/host/%.o: %.c src/lv_conf.h
	@mkdir -p $(dir $@)
	@$(HOST_CC) $(HOST_CFLAGS) $(XFLAGS) -c $< -o $@

build/dev/%.o: %.c src/lv_conf.h
	@mkdir -p $(dir $@)
	@$(DEV_CC) $(DEV_CFLAGS) $(XFLAGS) -c $< -o $@

$(HOST_OBJ) $(DEV_OBJ): src/xmb.h src/xmb_bg.h src/xmb_player.h src/audio.h src/library.h src/playlist.h src/cover.h src/filesave.h

clean:
	rm -rf build

.PHONY: host device clean playtest libtest seektest
