# The bench's own small programs. They link eeye's sources as they are,
# so what they measure is eeye's code.
#
#   make -C bench                 builddir/bench/{ee_run,bus_cat}, for this machine
#   make -C bench kit             builddir/bench/kit-arm64/: the same two, static,
#                                 for the ROV's Pi (64-bit)
#
# CROSS is the Pi toolchain's prefix. The release binaries are built with
# Bootlin's (GCC 15.3); a distribution's aarch64 cross compiler works too,
# and kit-arm64/BUILD records which one it was, since an effect's timing
# depends on the compiler.
ROOT := ..
OUT := $(ROOT)/builddir/bench
CC ?= cc
CROSS ?= aarch64-linux-gnu-
CFLAGS ?= -O3
FLAGS := -std=gnu99 -Wall -Wextra -pedantic -pthread -fopenmp \
	-I$(ROOT)/src -D_FILE_OFFSET_BITS=64

EE_RUN_SRC := ee_run.c $(addprefix $(ROOT)/src/,auto_vis.c camera_ctrl.c \
	config.c conv.c effect_chain.c fbus.c img_stats.c point_opps.c \
	video_frame.c)
BUS_CAT_SRC := bus_cat.c $(ROOT)/src/fbus.c

.PHONY: all kit clean
all: $(OUT)/ee_run $(OUT)/bus_cat

$(OUT)/ee_run: $(EE_RUN_SRC)
	@mkdir -p $(OUT)
	$(CC) $(FLAGS) $(CFLAGS) -o $@ $^ -lrt -lm

$(OUT)/bus_cat: $(BUS_CAT_SRC)
	@mkdir -p $(OUT)
	$(CC) $(FLAGS) $(CFLAGS) -o $@ $^ -lrt -lm

kit: $(OUT)/kit-arm64/ee_run $(OUT)/kit-arm64/bus_cat
	@$(CROSS)gcc --version | head -1 > $(OUT)/kit-arm64/BUILD
	@echo "flags: $(FLAGS) $(CFLAGS) -static" >> $(OUT)/kit-arm64/BUILD
	@cat $(OUT)/kit-arm64/BUILD

$(OUT)/kit-arm64/ee_run: $(EE_RUN_SRC)
	@mkdir -p $(OUT)/kit-arm64
	$(CROSS)gcc $(FLAGS) $(CFLAGS) -static -o $@ $^ -lrt -lm

$(OUT)/kit-arm64/bus_cat: $(BUS_CAT_SRC)
	@mkdir -p $(OUT)/kit-arm64
	$(CROSS)gcc $(FLAGS) $(CFLAGS) -static -o $@ $^ -lrt -lm

clean:
	rm -rf $(OUT)
