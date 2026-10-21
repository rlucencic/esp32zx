# Host build of the Spectrum core: make -C components/zx/host
#   build/zx_host <128|plus2|plus3> [frames] [out.ppm]   (see host_main.c)
# The Z80 core, the uPD765 and diskspace's CP/M and TR-DOS counters are esp32emu's
ZX        := ..
Z80       := ../../../../esp32emu/components/z80
EMUDBG    := ../../../../esp32emu/components/emudbg
FDC       := ../../../../esp32emu/components/upd765
SPACE     := ../../../../esp32emu/components/diskspace
BUILD     := build
CFLAGS    := -O2 -g -Wall -Wextra -Wno-unused-parameter -DZ80_HOST_HOOK -DCONFIG_Z80_CONTENTION=1 \
             -I$(ZX)/include -I$(ZX) -I$(Z80)/include -I$(EMUDBG)/include -I$(FDC)/include -I$(SPACE)/include

ZX_SRCS   := $(wildcard $(ZX)/*.c)
ZX_OBJS   := $(patsubst $(ZX)/%.c,$(BUILD)/zx/%.o,$(ZX_SRCS)) $(BUILD)/z80.o $(BUILD)/z80_dbg.o $(BUILD)/emudbg.o $(BUILD)/emudbg_posix.o $(BUILD)/upd765.o $(BUILD)/cpm.o $(BUILD)/trdos.o

all: $(BUILD)/zx_host

$(BUILD)/zx_host: host_main.c $(ZX_OBJS)
	$(CC) $(CFLAGS) $^ -lm -lpthread -o $@

$(BUILD)/zx/%.o: $(ZX)/%.c $(wildcard $(ZX)/*.h $(ZX)/include/*.h) $(Z80)/include/z80.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(Z80)/%.c $(Z80)/include/z80.h $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(EMUDBG)/%.c $(EMUDBG)/include/emudbg.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/upd765.o: $(FDC)/upd765.c $(FDC)/include/upd765.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/%.o: $(SPACE)/%.c $(SPACE)/include/diskspace.h
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: all clean
