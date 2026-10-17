# Host build of the Spectrum core: make -C components/zx/host
#   build/zx_host <128|plus2|plus3> [frames] [out.ppm]   (see host_main.c)
# The Z80 core and the uPD765 are esp32emu's
ZX        := ..
Z80       := ../../../../esp32emu/components/z80
FDC       := ../../../../esp32emu/components/upd765
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DZ80_HOST_HOOK -DCONFIG_Z80_CONTENTION=1 \
             -I$(ZX)/include -I$(ZX) -I$(Z80)/include -I$(FDC)/include

ZX_SRCS   := $(wildcard $(ZX)/*.c)
ZX_OBJS   := $(patsubst $(ZX)/%.c,$(BUILD)/zx/%.o,$(ZX_SRCS)) $(BUILD)/z80.o $(BUILD)/upd765.o

all: $(BUILD)/zx_host

$(BUILD)/zx_host: host_main.c $(ZX_OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/zx/%.o: $(ZX)/%.c $(wildcard $(ZX)/*.h $(ZX)/include/*.h) $(Z80)/include/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/z80.o: $(Z80)/z80.c $(Z80)/include/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/upd765.o: $(FDC)/upd765.c $(FDC)/include/upd765.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
