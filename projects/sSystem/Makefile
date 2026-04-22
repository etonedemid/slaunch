#---------------------------------------------------------------------------------
# sSystem Makefile — builds a Nintendo Switch system applet NSP
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
CFLAGS   := -g -Wall -O2 -ffunction-sections $(ARCH) \
            $(INCLUDE) -D__SWITCH__ -DSL_VERSION=\"$(APP_VERSION)\"

CXXFLAGS := $(CFLAGS) -std=c++20 -fno-rtti -fno-exceptions

ASFLAGS  := -g $(ARCH)

LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(TARGET).map

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
            -I$(LIBNX)/include -I$(PORTLIBS)/include

VPATH    := $(SOURCES) ../../libs/sCommon/source/sl/os

#---------------------------------------------------------------------------------
.PHONY: all clean install

all: $(OUTDIR)/main

$(OUTDIR)/main: $(BUILD)/$(TARGET).nso | $(OUTDIR)
	@cp $< $@
	@echo "NSO installed to $@"

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
