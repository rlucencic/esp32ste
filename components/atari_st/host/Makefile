# Host build of the STE core: make -C components/atari_st/host run TOS=roms/tos162.img
ROOT      := ../../..
MUSASHI   := ../../../../esp32emu/components/musashi
ST        := ..
BUILD     := build
CFLAGS    := -O2 -g -Wall -DM68K_INSTRUCTION_HOOK=2 -I$(ST)/include -I$(ST) -I$(MUSASHI) -I$(MUSASHI)/musashi -I$(MUSASHI)/generated
TOS       ?= $(ROOT)/roms/tos162.img
FRAMES    ?= 600
RAM_KB    ?= 1024
MONITOR   ?= color

ST_SRCS   := $(wildcard $(ST)/*.c)
M68K_SRCS := $(MUSASHI)/musashi/m68kcpu.c $(MUSASHI)/musashi/m68kdasm.c \
             $(MUSASHI)/musashi/softfloat/softfloat.c $(MUSASHI)/generated/m68kops.c
ST_OBJS   := $(patsubst $(ST)/%.c,$(BUILD)/st/%.o,$(ST_SRCS))
M68K_OBJS := $(patsubst $(MUSASHI)/%.c,$(BUILD)/m68k/%.o,$(M68K_SRCS))

run: $(BUILD)/st_host
	./$(BUILD)/st_host $(TOS) $(FRAMES) $(BUILD)/screen.ppm $(RAM_KB) $(MONITOR)
	sips -s format png $(BUILD)/screen.ppm --out $(BUILD)/screen.png >/dev/null

$(BUILD)/st_host: host_main.c $(ST_OBJS) $(M68K_OBJS)
	$(CC) $(CFLAGS) $^ -lm -o $@

$(BUILD)/st/%.o: $(ST)/%.c $(wildcard $(ST)/*.h $(ST)/include/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(BUILD)/m68k/%.o: $(MUSASHI)/%.c $(MUSASHI)/m68kconf.h $(wildcard $(MUSASHI)/musashi/*.h)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

clean:
	rm -rf $(BUILD)

.PHONY: run clean
