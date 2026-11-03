# Electric Eye -- GNU make build.
#
#   make                   builddir/eeye, optimized with debug info (-O2 -g)
#   make BUILD=release     builddir/eeye, -O3, no debug info
#   make BUILD=debug       builddir/eeye-d, unoptimized
#   make check             build and run every test suite (-j is fine)
#   make clean
#
# Written for GNU make 3.81 and a C99 compiler (GCC 4.7+ or clang), so it
# builds on old distros as well as new ones -- Debian 7 and up. Nothing
# here needs make 4.x: no !=, no .ONESHELL, no $(file).
#
# Everything is overridable from the command line or environment: CC,
# HOSTCC, CFLAGS, CPPFLAGS, LDFLAGS, LDLIBS, BUILDDIR, PKG_CONFIG, and
#   OPENMP=0|1                   default: on if $(CC) can build with it
#   TURBOJPEG_CFLAGS/_LIBS       default: pkg-config, else -lturbojpeg
# e.g. a libjpeg-turbo built from source into /opt/ljt:
#   make TURBOJPEG_CFLAGS=-I/opt/ljt/include \
#        TURBOJPEG_LIBS='/opt/ljt/lib/libturbojpeg.a'

BUILDDIR ?= builddir
BUILD ?= debugoptimized
PKG_CONFIG ?= pkg-config
# Builds tools that run during the build (the page embedder); differs from
# CC only when cross-compiling.
HOSTCC ?= cc

ifeq ($(BUILD),debugoptimized)
OPTFLAGS := -O2 -g
EXE := eeye
HEXE := hellion
else ifeq ($(BUILD),release)
OPTFLAGS := -O3
EXE := eeye
HEXE := hellion
else ifeq ($(BUILD),debug)
OPTFLAGS := -O0 -g
EXE := eeye-d
HEXE := hellion-d
else
$(error BUILD must be debugoptimized, release or debug, not '$(BUILD)')
endif

# One object directory per build type, so switching BUILD never links
# objects compiled with another type's flags.
OBJDIR := $(BUILDDIR)/obj-$(BUILD)
TESTDIR := $(BUILDDIR)/tests-$(BUILD)

CFLAGS ?= $(OPTFLAGS)
# _FILE_OFFSET_BITS=64: recordings pass 2GB quickly, and on 32-bit ARM
# (most Pis) off_t is 32 bits without it.
EE_CPPFLAGS := -Isrc -D_FILE_OFFSET_BITS=64
EE_CFLAGS := -std=gnu99 -Wall -Wextra -pedantic -pthread

# OpenMP: the #pragma omp loops in conv.c/point_opps.c are written to be
# race-free, but without -fopenmp every one of them is silently ignored and
# runs single-threaded. Probed with omp.h included, since clang can accept
# -fopenmp yet have no OpenMP runtime installed to go with it.
# (HASH: make 4.3 changed how "#" inside $(shell ...) is escaped; a
# variable holding one reads the same in every version.)
HASH := \#
ifeq ($(origin OPENMP),undefined)
OPENMP := $(shell printf '$(HASH)include <omp.h>\nint main(void) { return omp_get_max_threads() < 1; }\n' | \
            $(CC) -fopenmp -x c - -o /dev/null >/dev/null 2>&1 && echo 1 || echo 0)
endif
ifeq ($(OPENMP),1)
EE_CFLAGS += -fopenmp
else
$(warning OpenMP not available: conv.c/point_opps.c will build, but their \
  loops will run single-threaded.)
endif

# NEON on 32-bit ARM. conv.c and point_opps.c have NEON paths, compiled in
# only when the compiler defines __ARM_NEON -- and the compilers 32-bit Pi
# OS and Debian armhf ship do not by default (armv6 + VFP, or armv7 with
# VFPv3-D16), so a plain `make` there ran every effect scalar: ~3.5-6x the
# instructions for sobel, blur and LoG. On 32-bit hard-float ARM without
# NEON already on, this adds what the release armv7 build uses, if the
# compiler takes it. The binary then needs an ARMv7 with NEON and VFPv4:
# every Pi from the Pi 2 v1.1 on, not the Pi 1 or Zero. NEON=0 leaves the
# compiler's defaults alone. 64-bit ARM always has NEON; x86 is unaffected.
ifeq ($(origin NEON),undefined)
NEON := 1
endif
ifeq ($(NEON),1)
ARM_NEON_FLAGS := -march=armv7-a -mfpu=neon-vfpv4 -mfloat-abi=hard
NEED_NEON_FLAGS := $(shell echo | $(CC) $(CFLAGS) -dM -E - 2>/dev/null | \
  grep -q '__ARM_PCS_VFP' && ! (echo | $(CC) $(CFLAGS) -dM -E - 2>/dev/null | \
  grep -q '__ARM_NEON') && echo 'int main(void){return 0;}' | \
  $(CC) $(ARM_NEON_FLAGS) -x c - -o /dev/null >/dev/null 2>&1 && echo 1)
ifeq ($(NEED_NEON_FLAGS),1)
EE_CFLAGS += $(ARM_NEON_FLAGS)
endif
endif

# libturbojpeg (libjpeg-turbo 1.4 or newer: that's where the planar-YUV
# API this uses first appeared). Older distros' pkg-config may not know the
# library even when it is installed, hence the bare -lturbojpeg fallback.
ifeq ($(origin TURBOJPEG_CFLAGS),undefined)
TURBOJPEG_CFLAGS := $(shell $(PKG_CONFIG) --cflags libturbojpeg 2>/dev/null)
endif
ifeq ($(origin TURBOJPEG_LIBS),undefined)
TURBOJPEG_LIBS := $(shell $(PKG_CONFIG) --libs libturbojpeg 2>/dev/null || echo -lturbojpeg)
endif

# -lrt: clock_gettime() lived in librt until glibc 2.17; on anything newer
# it's an empty stub, so linking it unconditionally costs nothing.
EE_LDLIBS := $(TURBOJPEG_LIBS) -pthread -lrt -lm

ALL_CPPFLAGS = $(EE_CPPFLAGS) $(TURBOJPEG_CFLAGS) $(CPPFLAGS)
ALL_CFLAGS = $(EE_CFLAGS) $(CFLAGS)
ALL_LDFLAGS = $(EE_LDFLAGS) $(LDFLAGS)
ALL_LDLIBS = $(EE_LDLIBS) $(LDLIBS)

SRC := \
	src/eeye.c \
	src/auto_vis.c \
	src/bus_in.c \
	src/camera_ctrl.c \
	src/child_proc.c \
	src/config.c \
	src/conv.c \
	src/effect_chain.c \
	src/fbus.c \
	src/frame_ring_buffer.c \
	src/health.c \
	src/img_stats.c \
	src/jpeg_decode.c \
	src/video_frame.c \
	src/point_opps.c \
	src/recorder.c \
	src/rpicam_in.c \
	src/v4l2_in.c \
	src/v4l2_out.c \
	src/video_threads.c \
	src/virtual_cam.c

# hellion: its own sources, plus the shared modules from src/ it reuses
# (the frame bus, child processes, camera capture, the loopback output).
HSRC := \
	hellion/bench.c \
	hellion/check.c \
	hellion/control.c \
	hellion/discovery.c \
	hellion/ffmpeg.c \
	hellion/hconfig.c \
	hellion/hlog.c \
	hellion/hstatus.c \
	hellion/http.c \
	hellion/jpeg.c \
	hellion/main.c \
	hellion/pipeline.c \
	hellion/hrecord.c \
	hellion/sinks.c \
	hellion/top.c \
	hellion/udp_relay.c \
	src/camera_ctrl.c \
	src/child_proc.c \
	src/fbus.c \
	src/jpeg_decode.c \
	src/rpicam_in.c \
	src/v4l2_in.c \
	src/v4l2_out.c \
	src/video_frame.c \
	src/virtual_cam.c

obj = $(patsubst %.c,$(OBJDIR)/%.o,$(1))

.PHONY: all check clean
all: $(BUILDDIR)/$(EXE) $(BUILDDIR)/$(HEXE)

$(BUILDDIR)/$(EXE): $(call obj,$(SRC))
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LDLIBS)

$(BUILDDIR)/$(HEXE): $(call obj,$(HSRC)) $(OBJDIR)/gen/page.o
	$(CC) $(ALL_CFLAGS) $(ALL_LDFLAGS) -o $@ $^ $(ALL_LDLIBS)

# The web page goes inside the binary: hellion/web/index.html becomes a C
# array through a small tool built for the build machine (HOSTCC), so a
# cross build still runs it here.
$(BUILDDIR)/embed: hellion/embed.c
	@mkdir -p $(dir $@)
	$(HOSTCC) -O2 -o $@ $<

$(OBJDIR)/gen/page.c: hellion/web/index.html $(BUILDDIR)/embed
	@mkdir -p $(dir $@)
	$(BUILDDIR)/embed page_html $< $@

$(OBJDIR)/gen/page.o: $(OBJDIR)/gen/page.c
	$(CC) $(ALL_CFLAGS) -c -o $@ $<

$(OBJDIR)/hellion/%.o: EE_CPPFLAGS += -Ihellion

# -MMD -MP: each object also writes a .d file listing the headers it read,
# included below, so editing a header rebuilds exactly what uses it.
$(OBJDIR)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(ALL_CPPFLAGS) $(ALL_CFLAGS) -MMD -MP -c -o $@ $<

# Unit tests. Each includes the translation unit it exercises directly
# (rather than linking against it) so the file-static helpers -- the config
# parser, the JPEG frame splitter, the negotiation ranker -- are reachable
# without widening their visibility in the shipping build just to test them.
TESTS := config ring_buffer jpeg_framing negotiation jpeg_decode log_edges \
	canny health camera_ctrl recorder effect_chain child_proc \
	virtual_cam fbus hellion http auto_vis bench

# Extra translation units each test needs beyond the one it includes:
# rpicam_in.c and v4l2_in.c both reference the shared JPEG decoder and the
# frame pool, so those get linked in alongside.
test_config_SRC := src/camera_ctrl.c src/fbus.c
test_ring_buffer_SRC :=
test_jpeg_framing_SRC := src/camera_ctrl.c src/child_proc.c src/jpeg_decode.c \
	src/video_frame.c
test_negotiation_SRC := src/jpeg_decode.c src/video_frame.c
test_jpeg_decode_SRC := src/jpeg_decode.c src/video_frame.c
test_log_edges_SRC := src/conv.c src/video_frame.c
test_canny_SRC := src/conv.c src/video_frame.c
test_health_SRC :=
test_camera_ctrl_SRC :=
test_recorder_SRC := src/frame_ring_buffer.c src/health.c src/video_frame.c
test_child_proc_SRC :=
test_fbus_SRC :=
test_http_SRC :=
test_auto_vis_SRC := src/img_stats.c
test_bench_SRC := hellion/hstatus.c hellion/hlog.c src/fbus.c
test_hellion_SRC := hellion/discovery.c hellion/ffmpeg.c hellion/hconfig.c hellion/hlog.c \
	hellion/hstatus.c hellion/http.c hellion/jpeg.c hellion/udp_relay.c \
	hellion/pipeline.c hellion/hrecord.c hellion/sinks.c src/camera_ctrl.c \
	src/child_proc.c \
	src/fbus.c src/jpeg_decode.c src/rpicam_in.c src/v4l2_in.c src/v4l2_out.c \
	src/video_frame.c src/virtual_cam.c
test_virtual_cam_SRC := src/child_proc.c src/v4l2_out.c src/video_frame.c
test_effect_chain_SRC := src/auto_vis.c src/camera_ctrl.c src/config.c src/conv.c \
	src/fbus.c src/img_stats.c src/point_opps.c src/video_frame.c

$(OBJDIR)/tests/%.o: EE_CPPFLAGS += -Itests -Ihellion

define test_rule
$(TESTDIR)/test_$(1): $(OBJDIR)/tests/test_$(1).o $(call obj,$(test_$(1)_SRC))
	@mkdir -p $$(dir $$@)
	$$(CC) $$(ALL_CFLAGS) $$(ALL_LDFLAGS) -o $$@ $$^ $$(ALL_LDLIBS)
endef
$(foreach t,$(TESTS),$(eval $(call test_rule,$(t))))

# A hung suite fails instead of stalling the run, where coreutils has
# timeout (8.0+, so Debian 6 on).
TIMEOUT := $(shell command -v timeout >/dev/null 2>&1 && echo timeout)

# Each suite's output goes to a log beside its binary and is printed only
# on failure, so a parallel run stays readable.
define run_suite
	@if $(if $(TIMEOUT),$(TIMEOUT) $(2)) $(3) > $(TESTDIR)/$(1).log 2>&1; then \
	  echo "ok    $(1)"; \
	else \
	  echo "FAIL  $(1)"; cat $(TESTDIR)/$(1).log; exit 1; \
	fi
endef

CHECKS := $(addprefix check-,$(TESTS))
.PHONY: $(CHECKS)
$(CHECKS): check-%: $(TESTDIR)/test_%
	$(call run_suite,$*,60,$<)

check: $(CHECKS)

# Fuzzes hellion's HTTP parsers with libFuzzer (needs clang): the one part
# of hellion that parses bytes from the network. Not part of `check` --
# it runs until FUZZ_SECONDS is up. Seeds live in tests/fuzz_http_seeds/;
# anything new it finds goes into $(BUILDDIR)/fuzz-http-corpus/.
FUZZ_CC ?= clang
FUZZ_SECONDS ?= 60
.PHONY: fuzz-http
fuzz-http:
	@mkdir -p $(BUILDDIR)/fuzz-http-corpus
	$(FUZZ_CC) -g -O1 -std=gnu99 -Isrc -Ihellion \
	  -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined \
	  -o $(BUILDDIR)/fuzz_http tests/fuzz_http.c
	$(BUILDDIR)/fuzz_http -max_total_time=$(FUZZ_SECONDS) -max_len=9000 \
	  $(BUILDDIR)/fuzz-http-corpus tests/fuzz_http_seeds

clean:
	rm -rf $(BUILDDIR)

-include $(shell find $(BUILDDIR) -name '*.d' 2>/dev/null)
