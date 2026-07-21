#---------------------------------------------------------------------------------
# sInstaller Makefile - builds the sLaunch installer NRO.
# It bundles the built SD payload (from ../../SdOut) into its romfs and copies
# it onto the SD card, so no network/release is needed to install.
#---------------------------------------------------------------------------------

TARGET   := sInstaller
BUILD    := build
SOURCES  := source
INCLUDES := include

SDOUT    := ../../SdOut
OUTDIR   := $(SDOUT)/switch/sInstaller
ROMFS    := romfs

#---------------------------------------------------------------------------------
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := sLaunch Installer
APP_AUTHOR  := sLaunch
APP_VERSION := 0.7.0
APP_ICON    := ../../assets/icon.jpg

PKGCONF    := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
SDL_CFLAGS := $(shell $(PKGCONF) --cflags sdl2 SDL2_ttf SDL2_image)
SDL_LIBS   := $(shell $(PKGCONF) --libs sdl2 SDL2_ttf SDL2_image)
# libcurl (+ mbedtls/zlib) for the online update check/download.
CURL_CFLAGS := $(shell $(PKGCONF) --cflags libcurl)
CURL_LIBS   := $(shell $(PKGCONF) --libs libcurl)

#---------------------------------------------------------------------------------
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS    = -g -Wall -O2 -ffunction-sections $(ARCH) \
            $(INCLUDE) $(SDL_CFLAGS) $(CURL_CFLAGS) -D__SWITCH__ -DSL_VERSION=\"$(APP_VERSION)\"

CXXFLAGS  = $(CFLAGS) -std=c++20 -fno-rtti -fno-exceptions

LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(BUILD)/$(TARGET).map

# Network libs first (curl -> mbedtls -> z), minizip for extracting the update
# zip (needs -lz after it), then SDL.
LIBS     := $(CURL_LIBS) -lminizip $(SDL_LIBS) -lz

LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
CPPFILES := $(wildcard $(SOURCES)/*.cpp)
OFILES   := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(CPPFILES)))

INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
            -I$(LIBNX)/include -I$(PORTLIBS)/include

VPATH    := $(SOURCES)

#---------------------------------------------------------------------------------
.PHONY: all clean romfs_stage nxlink

all: romfs_stage $(OUTDIR)/$(TARGET).nro

# Stage the SD payload (everything except the installer itself) + the logo into
# the NRO's romfs.
romfs_stage:
	@echo "--- Staging installer payload ---"
	@rm -rf $(ROMFS)
	@mkdir -p $(ROMFS)/payload
	@cp -r $(SDOUT)/atmosphere $(ROMFS)/payload/ 2>/dev/null || true
	@cp -r $(SDOUT)/slaunch    $(ROMFS)/payload/ 2>/dev/null || true
	@cp ../../icon.png $(ROMFS)/icon.png 2>/dev/null || true

# Build the NRO with romfs + icon.
$(OUTDIR)/$(TARGET).nro: $(BUILD)/$(TARGET).nacp $(BUILD)/$(TARGET).elf | $(OUTDIR)
	@elf2nro $(BUILD)/$(TARGET).elf $@ --nacp=$(BUILD)/$(TARGET).nacp \
	    --romfsdir=$(ROMFS) \
	    $(if $(wildcard $(APP_ICON)),--icon=$(APP_ICON),)
	@echo "NRO: $@"

$(BUILD)/$(TARGET).nacp: | $(BUILD)
	@nacptool --create "$(APP_TITLE)" "$(APP_AUTHOR)" "$(APP_VERSION)" $@

$(BUILD)/$(TARGET).elf: $(OFILES) | $(BUILD)
	@$(CXX) $(LDFLAGS) -o $@ $^ \
	    $(foreach lib,$(LIBDIRS),-L$(lib)/lib) $(LIBS)

$(BUILD)/%.o: %.cpp | $(BUILD)
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

$(BUILD):
	@mkdir -p $@

$(OUTDIR):
	@mkdir -p $@

clean:
	@rm -rf $(BUILD) $(ROMFS) $(OUTDIR)/$(TARGET).nro

nxlink: all
	@nxlink -s $(OUTDIR)/$(TARGET).nro $(if $(SWITCH_IP),-a $(SWITCH_IP),)

-include $(OFILES:.o=.d)
