#---------------------------------------------------------------------------------
# sSystem Makefile - builds a Nintendo Switch system applet NSP
# Replaces qlaunch (title ID 0100000000001000) via Atmosphere content override.
# Output: ../../SdOut/atmosphere/contents/0100000000001000/exefs/main  (NSO)
#---------------------------------------------------------------------------------

TARGET   := sSystem
BUILD    := build
SOURCES  := source source/sl/sys/app source/sl/sys/la source/sl/sys/ecs source/sl/sys/smi
INCLUDES := include ../../libs/sCommon/include

COMMON_SRC := ../../libs/sCommon/source/sl/os/Applications.cpp

#---------------------------------------------------------------------------------
# Output path on SD card layout
SDOUT    := ../../SdOut
OUTDIR   := $(SDOUT)/atmosphere/contents/0100000000001000/exefs

#---------------------------------------------------------------------------------
# devkitPro toolchain
include $(DEVKITPRO)/libnx/switch_rules

#---------------------------------------------------------------------------------
# App metadata
APP_TITLE   := sSystem
APP_AUTHOR  := sLaunch
APP_VERSION := 0.1.0

#---------------------------------------------------------------------------------
# Compiler flags
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# libstratosphere headers select code paths on these defines; they must match
# the flags Atmosphere itself is built with (see libraries/config/*.mk).
ATMOSPHERE_DEFINES := -DATMOSPHERE -DATMOSPHERE_ARCH_ARM64 -DATMOSPHERE_ARCH_ARM_V8A \
                     -DATMOSPHERE_BOARD_NINTENDO_NX -DATMOSPHERE_OS_HORIZON -D__SWITCH__ \
                     -DATMOSPHERE_IS_STRATOSPHERE -D_GNU_SOURCE

CFLAGS    = -g -Wall -O2 -ffunction-sections -fdata-sections -fno-strict-aliasing -fwrapv \
            -fno-asynchronous-unwind-tables -fno-unwind-tables -fno-stack-protector \
            -Wno-format-zero-length -Wno-stringop-truncation -Wno-format-truncation \
            $(ARCH) $(INCLUDE) $(ATMOSPHERE_DEFINES) -DSL_VERSION=\"$(APP_VERSION)\"

CXXFLAGS  = $(CFLAGS) -std=gnu++23 -fno-rtti -fno-exceptions -Wno-invalid-offsetof

ASFLAGS  := -g $(ARCH)

# Build as a stratosphere sysmodule: libstratosphere's crt0 (via its specs)
# provides __appInit/main and calls our ams::init / ams::Main hooks, matching
# the sysmodule NPDM. The specs' linker script path is resolved from the
# ATMOSPHERE_LIBRARIES_DIR env var, so export it.
ATMOSPHERE_DIR ?= /opt/atmosphere
export ATMOSPHERE_LIBRARIES_DIR := $(ATMOSPHERE_DIR)/libraries
STRAT_SPECS := $(ATMOSPHERE_LIBRARIES_DIR)/libstratosphere/stratosphere.specs

CXXWRAPS := -Wl,--wrap,__cxa_pure_virtual -Wl,--wrap,__cxa_throw \
            -Wl,--wrap,__cxa_rethrow -Wl,--wrap,__cxa_allocate_exception \
            -Wl,--wrap,__cxa_free_exception -Wl,--wrap,__cxa_begin_catch \
            -Wl,--wrap,__cxa_end_catch -Wl,--wrap,__cxa_call_unexpected \
            -Wl,--wrap,__cxa_call_terminate -Wl,--wrap,__gxx_personality_v0 \
            -Wl,--wrap,_Unwind_Resume -Wl,--wrap,_ZSt19__throw_logic_errorPKc \
            -Wl,--wrap,_ZSt20__throw_length_errorPKc \
            -Wl,--wrap,_ZNSt11logic_errorC2EPKc -Wl,--wrap,exit

CXXREQUIRED := -Wl,--require-defined,__libnx_initheap \
               -Wl,--require-defined,__libnx_exception_handler \
               -Wl,--require-defined,__libnx_alloc \
               -Wl,--require-defined,__libnx_aligned_alloc \
               -Wl,--require-defined,__libnx_free \
               -Wl,--require-defined,__appInit \
               -Wl,--require-defined,__appExit \
               -Wl,--require-defined,argvSetup

LDFLAGS  := -specs=$(STRAT_SPECS) -specs=$(DEVKITPRO)/libnx/switch.specs \
            -g $(ARCH) $(CXXWRAPS) $(CXXREQUIRED) \
            -Wl,-Map,$(TARGET).map -Wl,-z,relro,-z,now

# Link against stratosphere (Atmosphere) for ECS + ldr:shel
LIBS     := -lstratosphere -lnx

LIBDIRS  := $(PORTLIBS) $(LIBNX) $(DEVKITPRO)/AtmosphereLibs

#---------------------------------------------------------------------------------
# Gather source files
CFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
CPPFILES := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp)) $(COMMON_SRC)
SFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.s))
OFILES   := $(patsubst %.cpp,$(BUILD)/%.o,$(CPPFILES)) \
            $(patsubst %.c,$(BUILD)/%.o,$(CFILES)) \
            $(patsubst %.s,$(BUILD)/%.o,$(SFILES))

INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
            -I$(DEVKITPRO)/AtmosphereLibs/include \
            -I$(LIBNX)/include -I$(PORTLIBS)/include

VPATH    := $(SOURCES) ../../libs/sCommon/source/sl/os

#---------------------------------------------------------------------------------
.PHONY: all clean install

all: $(OUTDIR)/main $(OUTDIR)/main.npdm

$(OUTDIR)/main: $(BUILD)/$(TARGET).nso | $(OUTDIR)
	@cp $< $@
	@echo "NSO installed to $@"

# qlaunch daemon NPDM (adapted from uLaunch's uSystem.json): program id
# 0100000000001000, application_type 2, full fs/service/svc access so the
# daemon can drive ldr:shel (ECS), pm, and serve the menu's exefs.
$(OUTDIR)/main.npdm: sSystem.json | $(OUTDIR)
	@npdmtool $< $@ > /dev/null 2>&1 || npdmtool $< $@
	@echo "NPDM installed to $@"

$(BUILD)/$(TARGET).nso: $(BUILD)/$(TARGET).elf
	@elf2nso $< $@

$(BUILD)/$(TARGET).elf: $(OFILES) | $(BUILD)
	@$(CXX) $(LDFLAGS) -o $@ $^ $(foreach lib,$(LIBDIRS),-L$(lib)/lib) $(LIBS)
	@$(NM) -CSn $@ > $(BUILD)/$(TARGET).lst

$(BUILD)/%.o: %.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/%.o: %.c | $(BUILD)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	@mkdir -p $@

$(OUTDIR):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(OUTDIR)/main

-include $(OFILES:.o=.d)
