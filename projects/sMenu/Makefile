#---------------------------------------------------------------------------------
# sMenu Makefile - builds a Nintendo Switch library applet NSO
# Loaded via Atmosphere ECS into the eShop applet slot.
# Output: ../../SdOut/slaunch/bin/sMenu/main  (NSO)
#---------------------------------------------------------------------------------

TARGET   := sMenu
BUILD    := build

SOURCES  := source source/sl/menu/ui source/sl/menu/gfx source/sl/menu/smi \
            source/sl/menu/net source/sl/menu/widgets source/sl/menu/audio \
            source/sl/menu/hb ../../libs/Lua

INCLUDES := include ../../libs/sCommon/include \
            ../../libs/Lua ../../libs/sol2/include

COMMON_SRC := ../../libs/sCommon/source/sl/os/Applications.cpp


# SDL2 (+ ttf/image) compile & link flags from the switch portlibs pkg-config.
PKGCONF  := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
SDL_CFLAGS := $(shell $(PKGCONF) --cflags sdl2 SDL2_ttf SDL2_image SDL2_mixer)
SDL_LIBS   := $(shell $(PKGCONF) --libs sdl2 SDL2_ttf SDL2_image SDL2_mixer)

# libcurl (+ mbedtls/zlib) for the network widgets (weather, chat).
CURL_CFLAGS := $(shell $(PKGCONF) --cflags libcurl)
CURL_LIBS   := $(shell $(PKGCONF) --libs libcurl)

#---------------------------------------------------------------------------------
# sMenu is a library applet: its exefs (main + main.npdm) lives in a folder on
# the SD card, which sSystem serves into an applet slot via ECS.
SDOUT  := ../../SdOut
OUTDIR := $(SDOUT)/slaunch/bin/sMenu

#---------------------------------------------------------------------------------
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := sMenu
APP_AUTHOR  := etonedemid
APP_VERSION := 0.7.0

#---------------------------------------------------------------------------------
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# --- Lua / sol2 configuration ---------------------------------------------------
# 1. Switch newlib lacks BSD _longjmp / _setjmp; map to standard variants.
# 2. Do NOT define LUA_USE_DLOPEN=0 - defined() checks are true even when 0.
# 3. Exclude standalone interpreter files (lua.c, luac.c) from the library build.
# 4. Force LUAI_MAXSTACK if llimits.h is not being included properly.
# 5. SOL_NO_EXCEPTIONS is mandatory because we compile with -fno-exceptions.
# 6. SOL_NO_PLAIN avoids sol2 trying to pass primitive refs by pointer.
# 7. If lua_newstate in your lua.h takes 3 args, change NEWSTATE_ARGS to 3.

NEWSTATE_ARGS := 2

LUA_DEFINES = -D__SWITCH__ \
              -D_longjmp=longjmp -D_setjmp=setjmp \
              -DLUAI_MAXSTACK=1000000 \
              -DLUA_32BITS \
              -DLUAI_FUNC=extern
			  
SOL_FLAGS = -DSOL_ALL_SAFETIES_ON=0 -DSOL_EXCEPTIONS_SAFE_PROPAGATION=0 \
            -DSOL_NO_EXCEPTIONS -DSOL_NO_PLAIN=1

COMPAT_FLAGS = -Dsol_lua_newstate_args=$(NEWSTATE_ARGS)

CFLAGS    = -g -Wall -O2 -ffunction-sections -fvisibility=default $(ARCH) \
            $(INCLUDE) $(SDL_CFLAGS) $(CURL_CFLAGS) $(LUA_DEFINES) \
            -DSL_VERSION=\"$(APP_VERSION)\"

CXXFLAGS = $(CFLAGS) -std=gnu++20 -fno-rtti -fno-exceptions \
           $(SOL_FLAGS) $(COMPAT_FLAGS)

ASFLAGS  := -g $(ARCH)

LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(TARGET).map \
            -Wl,--unresolved-symbols=ignore-in-object-files

# Network libs first (curl -> mbedtls -> z), then SDL (whose flags end with
# -lnx) so libnx resolves everything last.
LIBS     := $(CURL_LIBS) $(SDL_LIBS)

LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
CFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.c))
CFILES   := $(filter-out %/lua.c %/luac.c,$(CFILES))

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

all: $(OUTDIR)/main $(OUTDIR)/main.npdm

$(OUTDIR)/main: $(BUILD)/$(TARGET).nso | $(OUTDIR)
	@cp $< $@
	@echo "NSO installed to $@"

# Applet NPDM: hbl-style full permissions targeting the applet slot program id
# that sSystem's ECS redirects to us. Same manifest nx-hbloader runs homebrew
# under, so SDL/mesa + SD access work in the applet slot.
$(OUTDIR)/main.npdm: sMenu.json | $(OUTDIR)
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