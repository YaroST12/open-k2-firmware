# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
CC      = arm-none-eabi-gcc
CXX     = arm-none-eabi-g++
OBJCOPY = arm-none-eabi-objcopy
OBJDUMP = arm-none-eabi-objdump
SIZE    = arm-none-eabi-size
PYTHON  = python3

TARGET  = open_k2
BUILD_DIR = build
SRC_DIR = src
INC_DIR = include

# One firmware image serves every USB link mode (off, HID, Drive, HID+Drive); the mode is the stored USB link
# setting, chosen at boot (see docs/ARCHITECTURE.md).
#
# K2_NET=1 adds the network function (HID+Net: lwIP, TinyUSB's CDC-NCM class, the DHCP and HTTP servers and the web
# dashboard of tools/web). It is OFF by default: its RAM overlay (link.ld .mode_net) is the largest member of the
# USB-mode overlay and costs ~8 KB of RAM in every build, whether or not the mode is used. `make K2_NET=1` (or
# `make docker-build K2_NET=1`) builds it back; the last gate step (tools/gate.py) builds it in build-net/ so it keeps
# compiling and linking. See include/k2/core/build_config.h.
K2_NET ?= 0
# K2_WEB=1 puts the web parts back into `make test` and `make gate`: the node tests of the web pages (tools/web, webhid-flasher),
# the host tests of the network function (test_net_dhcp, test_usb_desc_net), the Python checks that read the web page's JS, and
# the gate's K2_NET=1 firmware variant. Off by default (2026-11-11: those parts are mostly unused; they get fixed in one go).
K2_WEB ?= 0
export K2_WEB

# K2_WERROR=1 (set by `make gate`): our own sources build with -Werror; third_party never does.
K2_WERROR ?= 0
ifeq ($(K2_WERROR),1)
OWN_WERROR = -Werror
endif

# Parallel builds by default for every entry point (make, docker-build, gate): the job count
# lives here, not at each call site. A command-line -j (e.g. `make -j1`) still wins over this.
K2_J ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
MAKEFLAGS += -j$(K2_J)

# Build identification shown by the remote PING command. Computed on the host and passed into
# the Docker build (the container has no git view of the worktree); only version.o depends on it.
# K2_FW_VERSION is ONE string, `<major>.<minor>.<patch>-<iteration>-<hash>[-dirty]` (tools/fw_version.py: the newest
# v* tag, the commit count since it, the short hash); K2_GIT stays the raw `git describe` for traceability.
K2_FW_VERSION ?= $(shell $(PYTHON) tools/fw_version.py)
K2_GIT ?= $(shell git describe --always --dirty 2>/dev/null || echo unknown)
GIT_STAMP = $(BUILD_DIR)/.git-$(K2_GIT)-$(K2_FW_VERSION)
# A missing identity stamp must rebuild version.o, even when .SECONDARY keeps host objects.
.NOTINTERMEDIATE: $(GIT_STAMP)

SRCS    = $(wildcard $(SRC_DIR)/*.c)
# Firmware code is C++23: every src/<group>/*.cpp (groups: drivers pd fastcharge chargertest ui core net, generated
# fonts in src/ui/fonts/); only third-party code is C. Objects mirror the tree under $(BUILD_DIR).
# The link order is the alphabetical order of the file names, the generated fonts last (the order the flat src/ had), so
# moving a file between groups does not reshuffle the image.
ALL_CXX_SRCS = $(shell find $(SRC_DIR) -name '*.cpp')
FONT_SRCS = $(sort $(filter $(SRC_DIR)/ui/fonts/%,$(ALL_CXX_SRCS)))
CODE_SRCS = $(filter-out $(FONT_SRCS),$(ALL_CXX_SRCS))
CXX_SRCS = $(foreach f,$(sort $(notdir $(CODE_SRCS))),$(filter %/$(f),$(CODE_SRCS))) $(FONT_SRCS)
NET_CXX_SRCS = $(SRC_DIR)/net/net.cpp $(SRC_DIR)/net/http_server.cpp   # the network function (K2_NET=1 only)
ifneq ($(K2_NET),1)
CXX_SRCS := $(filter-out $(NET_CXX_SRCS),$(CXX_SRCS))
endif
OBJS    = $(patsubst $(SRC_DIR)/%.c, $(BUILD_DIR)/%.o, $(SRCS)) \
          $(patsubst $(SRC_DIR)/%.cpp, $(BUILD_DIR)/%.o, $(CXX_SRCS))
ifeq ($(K2_NET),1)
OBJS   += $(BUILD_DIR)/web_asset.o
endif


# --- TinyUSB (git submodule, pinned to a release tag) ---
TUSB_DIR = third_party/tinyusb/src
TUSB_SRCS = \
	$(TUSB_DIR)/tusb.c \
	$(TUSB_DIR)/common/tusb_fifo.c \
	$(TUSB_DIR)/device/usbd.c \
	$(TUSB_DIR)/class/cdc/cdc_device.c \
	$(TUSB_DIR)/class/hid/hid_device.c \
	$(TUSB_DIR)/class/msc/msc_device.c \
	$(TUSB_DIR)/portable/st/stm32_fsdev/dcd_stm32_fsdev.c \
	$(TUSB_DIR)/portable/st/stm32_fsdev/fsdev_common.c
# TinyUSB networking helpers: DHCP server and RNDIS reports (they sit outside src/)
TUSB_NET_DIR = third_party/tinyusb/lib/networking
TUSB_NET_SRCS = $(TUSB_NET_DIR)/dhserver.c
TUSB_NCM_SRCS = $(TUSB_DIR)/class/net/ncm_device.c
# The sets above stay defined in every build (the host DHCP test compiles them); only K2_NET=1 links them.
ifeq ($(K2_NET),1)
TUSB_SRCS += $(TUSB_NCM_SRCS)
endif
TUSB_OBJS = $(patsubst $(TUSB_DIR)/%.c, $(BUILD_DIR)/tusb/%.o, $(TUSB_SRCS))
ifeq ($(K2_NET),1)
TUSB_OBJS += $(patsubst $(TUSB_NET_DIR)/%.c, $(BUILD_DIR)/tusb/net/%.o, $(TUSB_NET_SRCS))
endif
OBJS += $(TUSB_OBJS)

# --- lwIP (git submodule, pinned to a release tag); NO_SYS, options in include/lwipopts.h ---
LWIP_DIR = third_party/lwip/src
LWIP_SRCS = $(addprefix $(LWIP_DIR)/core/, init.c def.c inet_chksum.c ip.c mem.c memp.c netif.c \
              pbuf.c stats.c timeouts.c udp.c tcp.c tcp_in.c tcp_out.c) \
            $(addprefix $(LWIP_DIR)/core/ipv4/, etharp.c icmp.c ip4.c ip4_addr.c ip4_frag.c) \
            $(LWIP_DIR)/netif/ethernet.c
LWIP_OBJS = $(patsubst $(LWIP_DIR)/%.c, $(BUILD_DIR)/lwip/%.o, $(LWIP_SRCS))
ifeq ($(K2_NET),1)
OBJS += $(LWIP_OBJS)
endif

# --- littlefs (vendored, pinned release, see third_party/littlefs/PINNED.txt); options in include/k2/core/lfs_cfg.h ---
LFS_DIR = third_party/littlefs
LFS_SRCS = $(LFS_DIR)/lfs.c $(LFS_DIR)/lfs_util.c
LFS_OBJS = $(patsubst $(LFS_DIR)/%.c, $(BUILD_DIR)/littlefs/%.o, $(LFS_SRCS))
OBJS += $(LFS_OBJS)

# --- QR code generator (vendored, MIT, capped at version 10, see third_party/qrcodegen/PINNED.txt) ---
QR_DIR = third_party/qrcodegen
QR_SRCS = $(QR_DIR)/qrcodegen.c
QR_OBJS = $(patsubst $(QR_DIR)/%.c, $(BUILD_DIR)/qrcodegen/%.o, $(QR_SRCS))
OBJS += $(QR_OBJS)

ARCH_FLAGS = -mcpu=cortex-m4 -mthumb -mfloat-abi=hard -mfpu=fpv4-sp-d16
CFLAGS  =$(ARCH_FLAGS)
CFLAGS += -Os -g3 -Wall -Wextra -ffunction-sections -fdata-sections
# Identical constant objects share one copy. Not ISO-conforming (two const objects may get one
# address); nothing here compares the addresses of two const tables.
# Measured -800 B text under LTO.
CFLAGS += -fmerge-all-constants
CFLAGS += -I$(INC_DIR)
CFLAGS += -DK2_NET=$(K2_NET)
CFLAGS += -MMD -MP

# TinyUSB: include path + MCU selection; third-party code is built without -Wextra noise.
TUSB_CFLAGS = -isystem $(TUSB_DIR) -DCFG_TUSB_MCU=OPT_MCU_AT32F403A_407
CFLAGS += $(TUSB_CFLAGS)
CFLAGS += -I$(LWIP_DIR)/include -I$(TUSB_NET_DIR) -I$(LFS_DIR) -I$(QR_DIR)

# Bare-metal C++: no exceptions, RTTI, thread-safe statics or atexit registration; no heap.
CXXFLAGS  = $(CFLAGS) -std=c++23 -fno-exceptions -fno-rtti -fno-threadsafe-statics -fno-use-cxa-atexit
CXXFLAGS += -Wno-volatile

LDFLAGS = $(ARCH_FLAGS)
ifeq ($(K2_NET),1)
LDFLAGS += -Wl,--wrap=udp_sendto
endif
LDFLAGS += -Tlink.ld -Wl,--gc-sections -nostartfiles
LDFLAGS += -Wl,-Map=$(BUILD_DIR)/$(TARGET).map

# Link-time optimisation. The USB Net overlay (link.ld .mode_net) selects its .bss by object
# file name, which LTO's ltrans partitions would not keep, so the Net-only objects stay non-LTO.
# They exist in a K2_NET=1 build only.
LTO = -flto
NOLTO_OBJS =
ifeq ($(K2_NET),1)
NOLTO_OBJS = $(LWIP_OBJS) $(BUILD_DIR)/net/net.o $(BUILD_DIR)/net/http_server.o \
             $(BUILD_DIR)/tusb/class/net/ncm_device.o $(BUILD_DIR)/tusb/net/dhserver.o
$(NOLTO_OBJS): LTO =
endif
LDFLAGS += -flto=auto -Os -ffunction-sections -fdata-sections -fmerge-all-constants

# Extra flags for flag experiments and the stack report; empty in normal builds. Build such
# variants in their own BUILD_DIR (link.ld matches the Net objects by name under any directory).
CFLAGS += $(EXTRA_CFLAGS)
LDFLAGS += $(EXTRA_LDFLAGS)

# Call graph and per-function frame sizes for tools/stack_depth.py (the stack-depth gate step). Every build
# writes them: the LTO ltrans partitions into $(CG_DIR), the non-LTO objects (lwIP, Net) next to their .o.
# It only adds side files; the image is byte-identical.
CG_DIR = $(BUILD_DIR)/cg
CFLAGS += -fcallgraph-info=su,da
LDFLAGS += -fcallgraph-info=su,da -dumpdir $(CG_DIR)/

# Stack budget (docs/ARCHITECTURE.md "Stack"). Frames are sized when the LTO ltrans partitions are compiled, so
# the warnings belong on the link line (the per-file compile of an LTO object has no frame yet). A frame over
# FRAME_MAX trips -Wframe-larger-than / -Wstack-usage; under K2_WERROR=1 (the gate) it is an error. No third_party
# function is anywhere near the limit (largest 300 B), so the link-wide flag only ever names our own code.
# TODO: ratchet FRAME_MAX to 512 once the remaining big page and shell frames are split (tools/stack_depth.py
# lists the largest frames).
FRAME_MAX = 1024
LDFLAGS += -Wframe-larger-than=$(FRAME_MAX) -Wstack-usage=$(FRAME_MAX)
ifeq ($(K2_WERROR),1)
LDFLAGS += -Werror=frame-larger-than=$(FRAME_MAX) -Werror=stack-usage=$(FRAME_MAX)
endif
# Inlining is what makes a page's update() a 2 KB frame: -Os merges every one-call-site helper into it and the
# callee's locals all stay allocated. Cap the growth of an inlined caller's frame (measured: -500 B worst-case
# depth for +780 B text, better than -fconserve-stack's -220 B for +2.2 KB).
STACK_PARAMS = --param=large-stack-frame=256 --param=large-stack-frame-growth=100
CFLAGS += $(STACK_PARAMS)
LDFLAGS += $(STACK_PARAMS)

# Size flags, measured one by one and combined on the integration tip (text 564392 B, `make gate` stack
# numbers unchanged within 72 B; runs/size/exp_results.txt, tools/size_report.py for the per-function view):
#   -flto-partition=one       -3180 B  ltrans in one partition: cross-partition duplicates merge, one LTO
#                                      link process instead of parallel ones (the link is single-threaded)
#   -fno-schedule-insns2      -1432 B  the post-RA scheduler lengthens live ranges and costs spills; on the
#                                      in-order M4 pipeline it buys next to nothing
#   -fno-partial-inlining      -804 B  no hot/cold function splitting (.part.N clones that -Os rarely pays back)
# Together -5440 B (-1.0 %) with main-loop depth 4872 -> 4816. Tried and rejected: every global inline limit
# (max-inline-insns-single/auto, inline-unit-growth, -finline-limit) and -fno-inline-small-functions grow the
# text; -fno-inline-functions-called-once is +1.3 KB but cuts the main-loop worst case by 340 B and the fault
# path by 150 B (the option to take if the stack budget gets tight).
SIZE_FLAGS = -fno-schedule-insns2 -fno-partial-inlining
CFLAGS += $(SIZE_FLAGS)
LDFLAGS += $(SIZE_FLAGS) -flto-partition=one

DOCKER_IMG = open-k2-builder:latest

.PHONY: all clean flash probe reboot nav-headers docker-build docker-flash test web web-sync webhid gate smoke stack-report

all: $(BUILD_DIR)/$(TARGET).k2
	@$(COMPDB)

# compile_commands.json at the repo root, merged from the <object>.cmd fragments of build/ (firmware) and host-build/
# (tests) by tools/compdb.py; see "Code navigation" in docs/DEVELOPING.md. Only the default build directory owns the
# file: build-net/ and experiment directories (BUILD_DIR=...) leave it alone. K2_HOST_ROOT is the repo's host path
# (docker-build passes it; the container sees the repo as /workspace); it becomes `directory` in every entry.
K2_HOST_ROOT ?= $(CURDIR)
ifeq ($(BUILD_DIR),build)
COMPDB = K2_HOST_ROOT=$(K2_HOST_ROOT) $(PYTHON) tools/compdb.py
else
COMPDB = :
endif
.PHONY: compdb
compdb:
	@$(COMPDB)

# K2LOG format table (include/k2/core/log.hpp): tools/logdb.py scans the sources, extends the
# append-only tools/logdb.json and rewrites the header only when it changes.
LOGDB_HDR = $(BUILD_DIR)/gen/k2_logdb.hpp
CFLAGS += -I$(BUILD_DIR)/gen
$(LOGDB_HDR): $(CXX_SRCS) $(wildcard $(INC_DIR)/*.hpp $(INC_DIR)/k2/*.hpp $(INC_DIR)/k2/*/*.hpp) tools/logdb.py | $(BUILD_DIR)
	@$(PYTHON) tools/logdb.py gen $@

# Objects are not tracked against flags, so the stamp file is named after a hash of every compiler
# and linker setting (tools, CFLAGS, CXXFLAGS, LDFLAGS, -Werror, LTO); any flag edit or K2_WERROR /
# EXTRA_*FLAGS change gives a new stamp, which deletes the old one and rebuilds and relinks everything.
# Defined after the last global flag edit above; target-specific flags (version.o) are not part of it.
CFG_FLAGS := $(CC) | $(CXX) | $(CFLAGS) | $(CXXFLAGS) | $(LDFLAGS) | $(OWN_WERROR) | $(LTO) | $(NOLTO_OBJS)
CFG_STAMP := $(BUILD_DIR)/.cfg-$(word 1,$(shell printf '%s' '$(subst ','"'"',$(CFG_FLAGS))' | md5sum))
# A missing stamp must rebuild (.SECONDARY would otherwise treat it as an intermediate and skip it).
.NOTINTERMEDIATE: $(CFG_STAMP)

# Compile database (compile_commands.json for clangd, tools/compdb.py): every compile recipe is `$(call RUN,<var>)`
# where <var> names the variable holding the full command. RUN writes that command to <object>.cmd (make's $(file),
# no extra process; the directory is created first) and expands to the very same text for the shell to run, so the
# recorded command and the executed one cannot drift. The variables are recursive: $< $@ and the target-specific
# flags (version.o, -DK2_HOST_PINS, ...) resolve per target when the recipe runs.
RUN = $(shell mkdir -p $(dir $@))$(file >$@.cmd,$($(1)))$($(1))

FW_CC_CMD    = $(CC) $(CFLAGS) $(LTO) $(OWN_WERROR) -c $< -o $@
FW_CXX_CMD   = $(CXX) $(CXXFLAGS) $(LTO) $(OWN_WERROR) -c $< -o $@
TP_CC_CMD    = $(CC) $(CFLAGS) $(LTO) -Wno-unused-parameter -c $< -o $@
LFS_CC_CMD   = $(CC) $(CFLAGS) $(LTO) -Wno-unused-parameter -Wno-cast-function-type -include k2/core/lfs_cfg.h -c $< -o $@
QR_CC_CMD    = $(CC) $(CFLAGS) $(LTO) -DNDEBUG -Wno-unused-parameter -c $< -o $@

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c $(CFG_STAMP) | $(BUILD_DIR) $(LOGDB_HDR)
	@echo "  CC    $<"
	@$(call RUN,FW_CC_CMD)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.cpp $(CFG_STAMP) | $(BUILD_DIR) $(LOGDB_HDR)
	@echo "  CXX   $<"
	@$(call RUN,FW_CXX_CMD)

# The dashboard served over USB networking: tools/web/k2.html, gzip-compressed into flash.
$(BUILD_DIR)/web_asset.cpp: tools/web/k2.html tools/embed_web.py | $(BUILD_DIR)
	@echo "  EMBED $<"
	@$(PYTHON) tools/embed_web.py $< $@

$(BUILD_DIR)/web_asset.o: $(BUILD_DIR)/web_asset.cpp $(CFG_STAMP)
	@echo "  CXX   $<"
	@$(call RUN,FW_CXX_CMD)

$(BUILD_DIR)/core/version.o: CXXFLAGS += -DK2_FW_VERSION='"$(K2_FW_VERSION)"' -DK2_GIT_HASH='"$(K2_GIT)"'
$(BUILD_DIR)/core/version.o: $(GIT_STAMP)

$(GIT_STAMP): | $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.git-*
	@touch $@

$(BUILD_DIR)/tusb/%.o: $(TUSB_DIR)/%.c $(CFG_STAMP) | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(call RUN,TP_CC_CMD)

$(BUILD_DIR)/tusb/net/%.o: $(TUSB_NET_DIR)/%.c $(CFG_STAMP) | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(call RUN,TP_CC_CMD)

$(BUILD_DIR)/littlefs/%.o: $(LFS_DIR)/%.c $(CFG_STAMP) include/k2/core/lfs_cfg.h | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(call RUN,LFS_CC_CMD)

# NDEBUG: the library's argument asserts are programmer checks; none can fire for the fixed, bounded call in src/ui/qr.cpp.
$(BUILD_DIR)/qrcodegen/%.o: $(QR_DIR)/%.c $(CFG_STAMP) | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(call RUN,QR_CC_CMD)

$(BUILD_DIR)/lwip/%.o: $(LWIP_DIR)/%.c $(CFG_STAMP) | $(BUILD_DIR)
	@echo "  CC    $<"
	@$(call RUN,TP_CC_CMD)

-include $(OBJS:.o=.d)

$(BUILD_DIR)/$(TARGET).elf: $(OBJS) link.ld $(CFG_STAMP)
	@echo "  LD    $@"
	@mkdir -p $(CG_DIR) && rm -f $(CG_DIR)/*.ci
	@$(CXX) $(OBJS) $(LDFLAGS) -o $@
	@$(SIZE) $@

$(BUILD_DIR)/$(TARGET).bin: $(BUILD_DIR)/$(TARGET).elf
	@echo "  BIN   $@"
	@$(OBJCOPY) -O binary $< $@

$(BUILD_DIR)/$(TARGET).k2: $(BUILD_DIR)/$(TARGET).bin
	@echo "  PACK  $@"
	@$(PYTHON) tools/k2_flasher.py pack $< $@ --version $(K2_FW_VERSION)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

$(CFG_STAMP): | $(BUILD_DIR)
	@rm -f $(BUILD_DIR)/.cfg-*
	@touch $@

clean:
	@rm -rf $(HOST_DIR)
	@rm -rf $(BUILD_DIR) build-net
	@echo "Cleaned build artifacts."

# Host unit tests (pure logic only): each tests/test_*.c is one program, built with the host
# compiler and linked against TEST_LIB_SRCS. A new test file just appends the sources it needs.
HOST_DIR      = host-build
HOST_CC       ?= gcc
HOST_CFLAGS   = -std=c11 -Wall -Wextra -O0 -g -I$(INC_DIR) -Itests
TEST_LIB_SRCS =
TEST_SRCS     = $(wildcard tests/test_*.c)
TEST_BINS     = $(patsubst tests/%.c, $(HOST_DIR)/%, $(TEST_SRCS))

HOST_TESTC_CMD = $(HOST_CC) $(HOST_CFLAGS) $< $(TEST_LIB_SRCS) -o $@
$(HOST_DIR)/%: tests/%.c $(TEST_LIB_SRCS) $(wildcard tests/*.h) $(wildcard $(INC_DIR)/*.h)
	@$(call RUN,HOST_TESTC_CMD)

HOST_CXX       ?= g++
# The host tests build with the network function in (K2_NET=1) because they cover its pure logic and
# the K2_NET=1 descriptors; test_usb_desc_nonet below is the one object built with K2_NET=0.
HOST_K2_NET    = 1
HOST_CXXFLAGS  = -std=c++23 -Wall -Wextra -O0 -g -I$(INC_DIR) -Itests -I$(LFS_DIR) -I$(QR_DIR) -DK2_NET=$(HOST_K2_NET) -DK2_TRUNC_LOG=1
TEST_CXX_LIB   = $(SRC_DIR)/ui/ui.cpp $(SRC_DIR)/ui/ui_pages.cpp $(SRC_DIR)/ui/ui_detail.cpp $(SRC_DIR)/ui/capture.cpp tests/ui_stubs.cpp $(SRC_DIR)/ui/gfx.cpp $(SRC_DIR)/ui/rle565.cpp tests/fake_lcd.cpp $(SRC_DIR)/core/remote_proto.cpp $(SRC_DIR)/core/fmt.cpp $(SRC_DIR)/pd/pd.cpp $(SRC_DIR)/pd/pd_engine.cpp $(SRC_DIR)/pd/pd_ext.cpp $(SRC_DIR)/pd/pd_vdm.cpp $(SRC_DIR)/pd/usb_link.cpp $(SRC_DIR)/pd/pd_link.cpp $(SRC_DIR)/drivers/power_calc.cpp $(SRC_DIR)/drivers/adc_calc.cpp $(SRC_DIR)/fastcharge/fastcharge_classify.cpp $(SRC_DIR)/ui/ui_calc.cpp $(SRC_DIR)/drivers/buttons.cpp $(SRC_DIR)/ui/ui_layout.cpp $(SRC_DIR)/ui/ui_labels.cpp $(SRC_DIR)/ui/list_view.cpp $(SRC_DIR)/pd/pd_source.cpp $(SRC_DIR)/pd/pd_sink.cpp $(SRC_DIR)/pd/pd_identify.cpp $(SRC_DIR)/pd/pd_probe.cpp $(SRC_DIR)/ui/ui_pdtrig.cpp $(SRC_DIR)/ui/ui_wizard.cpp $(SRC_DIR)/fastcharge/fc_engine.cpp $(SRC_DIR)/fastcharge/fc_scan.cpp $(SRC_DIR)/ui/text.cpp $(SRC_DIR)/pd/emrec.cpp $(SRC_DIR)/pd/emk_quirk.cpp $(SRC_DIR)/pd/emk_respond.cpp $(SRC_DIR)/pd/emk_learn.cpp $(SRC_DIR)/pd/emname.cpp $(SRC_DIR)/pd/emjson.cpp $(SRC_DIR)/drivers/vfat.cpp $(SRC_DIR)/drivers/drive.cpp $(SRC_DIR)/ui/ui_widgets.cpp $(SRC_DIR)/ui/ui_icons.cpp $(SRC_DIR)/ui/ui_match.cpp $(SRC_DIR)/ui/ui_lines.cpp $(SRC_DIR)/ui/ui_tools.cpp $(SRC_DIR)/ui/ui_emarker.cpp $(SRC_DIR)/ui/ui_cabler.cpp $(SRC_DIR)/ui/ui_connection_page.cpp $(SRC_DIR)/ui/ui_blackbox.cpp $(SRC_DIR)/ui/ui_spoof.cpp $(SRC_DIR)/ui/ui_cards.cpp $(SRC_DIR)/pd/pdtrace.cpp $(SRC_DIR)/pd/pdtrace_view.cpp $(SRC_DIR)/pd/pdtrace_link.cpp $(wildcard $(SRC_DIR)/ui/fonts/*.cpp)
TEST_CXX_SRCS  = $(wildcard tests/test_*.cpp)
TEST_CXX_LIB  += tests/trunc_log.cpp $(SRC_DIR)/ui/orientation.cpp $(SRC_DIR)/core/shell.cpp $(SRC_DIR)/core/lab.cpp
TEST_CXX_LIB  += $(SRC_DIR)/pd/pd_reread.cpp $(SRC_DIR)/pd/pd_survey.cpp $(SRC_DIR)/pd/apple_readout.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/ui_viewport.cpp $(SRC_DIR)/core/logfmt.cpp $(SRC_DIR)/core/scratch.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/ui_orientation.cpp $(SRC_DIR)/ui/kit.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/home_model.cpp $(SRC_DIR)/ui/home_cell.cpp $(SRC_DIR)/ui/home_proto.cpp $(SRC_DIR)/ui/home_cards.cpp $(SRC_DIR)/ui/ui_home.cpp
TEST_CXX_LIB  += $(SRC_DIR)/chargertest/report_card_json.cpp $(SRC_DIR)/chargertest/result_layout.cpp $(SRC_DIR)/chargertest/step_label.cpp $(SRC_DIR)/core/fs.cpp
TEST_CXX_LIB  += $(SRC_DIR)/chargertest/sweep.cpp $(SRC_DIR)/core/ringlog.cpp $(SRC_DIR)/core/fs_migrate.cpp
TEST_CXX_LIB  += $(SRC_DIR)/chargertest/charger_test.cpp $(SRC_DIR)/ui/ui_charger_test.cpp $(SRC_DIR)/ui/ui_cable_list.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/qr.cpp $(SRC_DIR)/ui/qrview.cpp $(SRC_DIR)/core/bootinfo.cpp $(SRC_DIR)/ui/bootscreen.cpp $(SRC_DIR)/ui/qrshare.cpp $(SRC_DIR)/ui/qrshare_records.cpp $(SRC_DIR)/ui/qrshare_link.cpp $(SRC_DIR)/ui/ui_share.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/ui_twin.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/ui_pdstress.cpp
TEST_CXX_LIB  += $(SRC_DIR)/loopprobe/loop_probe.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/ui_loop_probe.cpp
TEST_CXX_LIB  += $(SRC_DIR)/ui/ui_pdstress_page.cpp
TEST_CXX_LIB  += $(SRC_DIR)/pd/diag.cpp $(SRC_DIR)/pd/diag_screen.cpp $(SRC_DIR)/ui/ui_diagnosis.cpp
TEST_CXX_BINS  = $(patsubst tests/%.cpp, $(HOST_DIR)/%, $(TEST_CXX_SRCS))

# Every library source is compiled once into $(HOST_DIR)/obj/<set>/<path>.o (with -MMD -MP
# dependency files) and each test only compiles its own .cpp and links the objects. Plain
# objects, not an archive, so every member is linked exactly as the old source-list link did.
# Object sets stay separate where the flags differ: lib (default), scan (-DK2_HOST_PINS).
OBJ_DIR        = $(HOST_DIR)/obj
LIB_OBJS       = $(patsubst %.cpp, $(OBJ_DIR)/lib/%.o, $(TEST_CXX_LIB)) $(patsubst %.c, $(OBJ_DIR)/lfs/%.o, $(LFS_SRCS)) $(patsubst %.c, $(OBJ_DIR)/lfs/%.o, $(QR_SRCS))
# The QR generator is C as well, built the same way (its asserts off, as on the device).
HOST_QR_CMD    = $(HOST_CC) -c -O0 -g -w -DNDEBUG -MMD -MP -I$(QR_DIR) $< -o $@
HOST_LFS_CMD   = $(HOST_CC) -c -O0 -g -w -MMD -MP -I$(INC_DIR) -I$(LFS_DIR) -include k2/core/lfs_cfg.h $< -o $@
$(OBJ_DIR)/lfs/$(QR_DIR)/%.o: $(QR_DIR)/%.c
	@$(call RUN,HOST_QR_CMD)
# littlefs is C: compiled with the host cc and linked into every test (a few unused functions are dropped).
$(OBJ_DIR)/lfs/%.o: %.c include/k2/core/lfs_cfg.h
	@$(call RUN,HOST_LFS_CMD)
SCAN_TEST_LIB  = $(SRC_DIR)/drivers/fusb302.cpp $(SRC_DIR)/pd/pdtrace.cpp $(SRC_DIR)/pd/pdtrace_link.cpp $(SRC_DIR)/pd/pd.cpp $(SRC_DIR)/pd/pd_engine.cpp $(SRC_DIR)/pd/pd_ext.cpp $(SRC_DIR)/pd/pd_vdm.cpp $(SRC_DIR)/pd/usb_link.cpp $(SRC_DIR)/pd/pd_sink.cpp $(SRC_DIR)/pd/pd_identify.cpp $(SRC_DIR)/pd/pd_probe.cpp $(SRC_DIR)/pd/pd_source.cpp $(SRC_DIR)/pd/apple_readout.cpp
SCAN_TEST_LIB += $(SRC_DIR)/pd/pd_reread.cpp $(SRC_DIR)/pd/diag.cpp
SCAN_OBJS      = $(patsubst %.cpp, $(OBJ_DIR)/scan/%.o, $(SCAN_TEST_LIB))

HOST_CXX_CMD   = $(HOST_CXX) $(HOST_CXXFLAGS) -MMD -MP -c $< -o $@
HOST_SCAN_CMD  = $(HOST_CXX) $(HOST_CXXFLAGS) -DK2_HOST_PINS -MMD -MP -c $< -o $@
$(OBJ_DIR)/lib/%.o: %.cpp
	@$(call RUN,HOST_CXX_CMD)
$(OBJ_DIR)/scan/%.o: %.cpp
	@$(call RUN,HOST_SCAN_CMD)
$(OBJ_DIR)/tests/%.o: tests/%.cpp
	@$(call RUN,HOST_CXX_CMD)

# The descriptor tests include src/drivers/usb_descriptors.cpp, which needs TinyUSB's headers.
USB_DESC_TESTS = test_usb_desc_msc1 test_usb_desc_hidonly test_usb_desc_net test_usb_desc_nonet test_usb_desc_shell test_usb_pma
$(patsubst %, $(OBJ_DIR)/tests/%.o, $(USB_DESC_TESTS)): HOST_CXXFLAGS += -isystem $(TUSB_DIR) -DCFG_TUSB_MCU=OPT_MCU_AT32F403A_407
$(OBJ_DIR)/tests/test_usb_desc_nonet.o: HOST_K2_NET = 0

$(HOST_DIR)/%: $(OBJ_DIR)/tests/%.o $(LIB_OBJS)
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@

# The e-marker scan test runs the real src/drivers/fusb302.cpp against a fake chip, so it cannot share
# TEST_CXX_LIB (tests/ui_stubs.cpp stubs the fusb302 API); pins are logged via the K2_HOST_PINS seam.
$(OBJ_DIR)/tests/test_fusb302_scan.o: HOST_CXXFLAGS += -DK2_HOST_PINS
$(HOST_DIR)/test_fusb302_scan: $(OBJ_DIR)/tests/test_fusb302_scan.o $(SCAN_OBJS)
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@

# The PD world replay (tests/test_world_replay.cpp, docs/TESTING.md "World replay") runs the real main loop, FUSB302 and
# power-monitor drivers and the whole UI against tests/world_sim.cpp. Its objects come FIRST on the link line: they are built
# with -DK2_HOST_PINS, so their copies of the inline pin accessors win over the lib set's. ui_stubs.cpp is rebuilt with
# -DK2_WORLD_STUBS (no clock, FUSB302, power, ADC or Rd-comeback stand-ins). The reboot world boots each life through the real
# PD half of boot() (src/core/boot_pd.cpp) and the real self-powered comeback (src/core/rd_comeback.cpp); the main loop runs the
# real HID-cable loop guard (src/core/loop_guard.cpp) on the world's aux rail and VBUS.
WORLD_SRCS    = tests/test_world_replay.cpp tests/world_sim.cpp tests/ui_stubs.cpp $(SRC_DIR)/core/main_loop.cpp
WORLD_SRCS    += $(SRC_DIR)/core/boot_pd.cpp $(SRC_DIR)/core/rd_comeback.cpp $(SRC_DIR)/core/loop_guard.cpp
WORLD_OBJS     = $(patsubst %.cpp, $(OBJ_DIR)/world/%.o, $(WORLD_SRCS)) $(OBJ_DIR)/scan/$(SRC_DIR)/drivers/fusb302.o
WORLD_OBJS    += $(patsubst %.cpp, $(OBJ_DIR)/lib/%.o, $(SRC_DIR)/drivers/power_monitor.cpp $(SRC_DIR)/drivers/accelerometer.cpp)
HOST_WORLD_CMD = $(HOST_CXX) $(HOST_CXXFLAGS) -DK2_HOST_PINS -DK2_WORLD_STUBS -MMD -MP -c $< -o $@
$(OBJ_DIR)/world/%.o: %.cpp
	@$(call RUN,HOST_WORLD_CMD)
$(HOST_DIR)/test_world_replay: $(WORLD_OBJS) $(filter-out $(OBJ_DIR)/lib/tests/ui_stubs.o, $(LIB_OBJS))
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@

# The spoof state test uses its fake receive/transmit hook, not the UI driver's stand-ins.
$(HOST_DIR)/test_emk_spoof: $(OBJ_DIR)/tests/test_emk_spoof.o $(patsubst %.cpp, $(OBJ_DIR)/lib/%.o, src/pd/emk_spoof.cpp src/pd/emk_quirk.cpp src/pd/emk_respond.cpp src/pd/emrec.cpp src/core/fmt.cpp)
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@

# The real accelerometer driver runs against this test's fake INA-bus transport.
$(HOST_DIR)/test_accelerometer: $(OBJ_DIR)/tests/test_accelerometer.o $(OBJ_DIR)/lib/src/drivers/accelerometer.o $(OBJ_DIR)/lib/src/ui/orientation.o
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@

# The DHCP/ARP test runs the real src/net/net.cpp, lwIP and TinyUSB dhserver on the host with a fake USB
# side. lwIP is C, so each source is compiled with the host cc first and linked into the C++ test.
NETTEST_C      = $(LWIP_SRCS) $(TUSB_NET_SRCS)
NETTEST_OBJS   = $(patsubst %.c, $(HOST_DIR)/nettest/%.o, $(NETTEST_C))
NET_FLAGS      = -I$(LWIP_DIR)/include -I$(TUSB_NET_DIR) -isystem $(TUSB_DIR) -DCFG_TUSB_MCU=OPT_MCU_AT32F403A_407
HOST_NETC_CMD  = $(HOST_CC) -c -O0 -g -w -MMD -MP -I$(INC_DIR) -I$(LWIP_DIR)/include -I$(TUSB_NET_DIR) $< -o $@
$(HOST_DIR)/nettest/%.o: %.c include/lwipopts.h
	@$(call RUN,HOST_NETC_CMD)
$(OBJ_DIR)/tests/test_net_dhcp.o $(OBJ_DIR)/net/net.o: HOST_CXXFLAGS += $(NET_FLAGS)
$(OBJ_DIR)/net/net.o: $(SRC_DIR)/net/net.cpp
	@$(call RUN,HOST_CXX_CMD)
$(OBJ_DIR)/net/mode_ram.o: $(SRC_DIR)/drivers/mode_ram.cpp
	@$(call RUN,HOST_CXX_CMD)
$(HOST_DIR)/test_net_dhcp: $(OBJ_DIR)/tests/test_net_dhcp.o $(OBJ_DIR)/net/net.o $(OBJ_DIR)/net/mode_ram.o $(NETTEST_OBJS)
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -Wl,--wrap=udp_sendto -o $@

# The Black Box diagnosis as a host tool (tools/pd_trace.py diagnose runs it): the device's own analyser
# (src/pd/diag.cpp) fed from a saved capture, so there is no second analyser in Python.
BBDIAG_OBJS    = $(patsubst %.cpp, $(OBJ_DIR)/lib/%.o, $(SRC_DIR)/pd/diag.cpp $(SRC_DIR)/pd/pdtrace_link.cpp $(SRC_DIR)/pd/pd.cpp)
$(OBJ_DIR)/tools/%.o: tools/%.cpp
	@$(call RUN,HOST_CXX_CMD)
$(HOST_DIR)/bbdiag: $(OBJ_DIR)/tools/bbdiag.o $(BBDIAG_OBJS)
	@$(HOST_CXX) $(HOST_CXXFLAGS) $^ -o $@
.PHONY: bbdiag
bbdiag: $(HOST_DIR)/bbdiag

-include $(shell find $(HOST_DIR)/obj $(HOST_DIR)/nettest -name '*.d' 2>/dev/null)

# Test run step: one stamp per binary, so tests run in parallel under `make -jN test` and an
# unchanged test is not re-run. Output is grouped per test (-Otarget) and a failure stops make.
# C++ tests share no state except K2_DUMP files, which only the Python suites read afterwards.
.SECONDARY:
.PHONY: test-run test-py
TEST_ALL_BINS  = $(TEST_BINS) $(TEST_CXX_BINS)
ifneq ($(K2_WEB),1)
TEST_ALL_BINS := $(filter-out $(HOST_DIR)/test_net_dhcp $(HOST_DIR)/test_usb_desc_net, $(TEST_ALL_BINS))
endif
TEST_STAMPS    = $(patsubst $(HOST_DIR)/%, $(HOST_DIR)/run/%.ok, $(TEST_ALL_BINS))
$(HOST_DIR)/run/%.ok: $(HOST_DIR)/%
	@mkdir -p $(dir $@)
	@K2_DUMP=$(HOST_DIR) $< && touch $@
test-run: $(TEST_STAMPS)
	@$(COMPDB)
test-py: test-run $(HOST_DIR)/bbdiag
	@python3 -m unittest discover -s tests -p 'test_*.py'
	# --test-timeout is only a hang guard: the functional node rigs disable their own wall-clock watchdogs
	# (count work, not time), so a rig that stops replying would otherwise hang `make gate` forever. 10 min is
	# far beyond any load-induced slowness of these tests, so the guard itself cannot flake.
	@if [ "$(K2_WEB)" != 1 ]; then echo "web tests off (K2_WEB=1 runs them)"; \
	elif command -v node >/dev/null 2>&1; then node --test --test-timeout=600000 tests/test_k2proto.mjs tests/test_k2boot.mjs tests/test_flasher_page.mjs tests/test_cardpage.mjs tests/test_recordspage.mjs tests/test_dataframes.mjs; \
	else echo "node not found: skipping node tests"; fi
test: test-py

# WebHID dashboard (tools/web/k2.html): WebHID needs a secure context, and http://localhost counts as one.
WEB_PORT ?= 8765
web:
	@echo "Open http://localhost:$(WEB_PORT)/k2.html in Chrome or Edge (Ctrl-C stops the server)"
	@(sleep 1; xdg-open http://localhost:$(WEB_PORT)/k2.html >/dev/null 2>&1 || true) &
	@python3 -m http.server --bind 127.0.0.1 -d tools/web $(WEB_PORT)

# k2.html embeds tools/web/k2proto.js; run this after editing the protocol file.
web-sync:
	@python3 tools/web/sync_proto.py

# WebHID firmware flasher (webhid-flasher/index.html): staged with tools/web/k2proto.js next to it, the
# layout .github/workflows/pages.yml publishes; nothing is tracked twice.
webhid:
	@mkdir -p $(BUILD_DIR)/webhid
	@cp webhid-flasher/index.html webhid-flasher/k2boot.js tools/web/k2proto.js $(BUILD_DIR)/webhid/
	@echo "Open http://localhost:$(WEB_PORT)/ in Chrome or Edge (Ctrl-C stops the server)"
	@(sleep 1; xdg-open http://localhost:$(WEB_PORT)/ >/dev/null 2>&1 || true) &
	@python3 -m http.server --bind 127.0.0.1 -d $(BUILD_DIR)/webhid $(WEB_PORT)

# The whole GitHub Pages site: .github/workflows/pages.yml builds the firmware, then runs this very target with
# SITE_DIR=_site, so the workflow and a local run cannot drift. Layout: the flasher at the root, docs/site/ copied over it
# (the results viewer at /r/), and, when $(BUILD_DIR)/open_k2.k2 exists, fw/open_k2.k2 + fw/latest.json (tools/stage_fw.py;
# the flasher's "Latest build" row reads them). Without a build the site is staged without fw/ and the row stays hidden.
# Open http://localhost:$(WEB_PORT)/r/#<link> to try a K2 results link.
SITE_DIR ?= $(BUILD_DIR)/site
.PHONY: site
site:
	@rm -rf $(SITE_DIR)
	@mkdir -p $(SITE_DIR)
	@cp webhid-flasher/index.html webhid-flasher/k2boot.js tools/web/k2proto.js $(SITE_DIR)/
	@cp -r docs/site/. $(SITE_DIR)/
	@$(PYTHON) tools/stage_fw.py --build $(BUILD_DIR) --out $(SITE_DIR) --optional
	@echo "Staged in $(SITE_DIR) (serve it: python3 -m http.server -d $(SITE_DIR) $(WEB_PORT))"

# Host Flashing Targets (Requires physical tester in DFU mode)
probe:
	@$(PYTHON) tools/k2_flasher.py probe

flash: $(BUILD_DIR)/$(TARGET).k2
	@$(PYTHON) tools/k2_flasher.py flash $< --auto-dfu $(FLASH_ARGS)

reboot:
	@$(PYTHON) tools/k2_flasher.py reboot

# Docker Targets
docker-image:
	docker build -t $(DOCKER_IMG) .

# Runs as the host uid so build/ stays owned by the caller (a root-owned build/ breaks later host runs).
DOCKER_RUN = docker run --rm --user $(shell id -u):$(shell id -g) -v $(CURDIR):/workspace -w /workspace

# Code navigation: the ARM compiler's system include directories, copied out of the build image into
# $(NAV_DIR)/root (dirs.txt lists them, as paths inside the image) so clangd on the host finds <cstdint> and the
# rest of newlib / libstdc++ (tools/compdb.py adds them to the firmware entries). Done once, by the first docker-build
# (a failure only costs the std headers); `make nav-headers` redoes it after a toolchain image change.
NAV_DIR = .cache/arm-none-eabi
$(NAV_DIR)/dirs.txt:
	-@rm -rf $(NAV_DIR) && mkdir -p $(NAV_DIR)/root \
	  && $(DOCKER_RUN) $(DOCKER_IMG) sh -c 'arm-none-eabi-g++ $(ARCH_FLAGS) -std=c++23 -x c++ -E -v /dev/null 2>&1 | sed -n "/^#include <...>/,/^End of search/p" | grep "^ /" | while read p; do realpath "$$p"; done' > $(NAV_DIR)/dirs.tmp \
	  && $(DOCKER_RUN) $(DOCKER_IMG) sh -c 'cd / && tar -cf - $$(sed "s|^/||" /workspace/$(NAV_DIR)/dirs.tmp)' | tar -C $(NAV_DIR)/root -xf - \
	  && mv $(NAV_DIR)/dirs.tmp $@ && echo "  NAV   $(NAV_DIR) (ARM system headers for clangd)"

nav-headers:
	@rm -rf $(NAV_DIR)
	@$(MAKE) --no-print-directory $(NAV_DIR)/dirs.txt
	@$(COMPDB)

docker-build: | $(NAV_DIR)/dirs.txt
	$(DOCKER_RUN) $(DOCKER_IMG) make K2_GIT=$(K2_GIT) K2_FW_VERSION=$(K2_FW_VERSION) K2_WERROR=$(K2_WERROR) K2_NET=$(K2_NET) BUILD_DIR=$(BUILD_DIR) K2_HOST_ROOT=$(CURDIR)

docker-flash:
	docker run --rm --privileged -v $(CURDIR):/workspace -v /dev:/dev $(DOCKER_IMG) make flash

# Pre-commit gate: -Werror build, invariants, map check, command ids, tests, size report.
gate:
	@$(PYTHON) tools/gate.py

# Gate, then flash and exercise the device (hands-free, read-only plus page navigation).
smoke: gate
	@$(PYTHON) tools/hw_smoke.py

# Worst-case stack per entry point from the call graph every build writes (build/cg + the non-LTO .ci files):
# the main-loop chain with every frame (page virtuals and list rows followed), each ISR, the largest frames.
# Run `make docker-build` first. Extra options: make stack-report STACK_ARGS="--root k2_fault_main --top 20".
stack-report:
	@$(PYTHON) tools/stack_depth.py --build $(BUILD_DIR) $(STACK_ARGS)
