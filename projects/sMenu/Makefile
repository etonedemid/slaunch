#---------------------------------------------------------------------------------
# sMenu Makefile - builds a Nintendo Switch library applet NSO
# Loaded via Atmosphere ECS into the eShop applet slot.
# Output: ../../SdOut/slaunch/bin/sMenu/main  (NSO)
#---------------------------------------------------------------------------------

TARGET   := sMenu
BUILD    := build

SOURCES  := source source/sl/menu/ui source/sl/menu/gfx source/sl/menu/smi \
            source/sl/menu/net source/sl/menu/widgets source/sl/menu/audio \
            source/sl/menu/hb source/sl/menu/play source/sl/menu/dbg \
            source/sl/menu/news source/sl/menu/cfg source/sl/menu/usb ../../libs/Lua

INCLUDES := include ../../libs/sCommon/include \
            ../../libs/Lua ../../libs/sol2/include ../../libs/haze/include

COMMON_SRC := ../../libs/sCommon/source/sl/os/Applications.cpp


# SDL2 (+ ttf/image) compile & link flags from the switch portlibs pkg-config.
PKGCONF  := $(DEVKITPRO)/portlibs/switch/bin/aarch64-none-elf-pkg-config
SDL_CFLAGS := $(shell $(PKGCONF) --cflags sdl2 SDL2_ttf SDL2_image SDL2_mixer)
SDL_LIBS   := $(shell $(PKGCONF) --libs sdl2 SDL2_ttf SDL2_image SDL2_mixer)
# SDL2_image before 2.8 decodes JPEGs with libjpeg, whose error path crashes
# on the console: one corrupt image takes the menu down. 2.8 uses stb_image.
# devkitPro still ships 2.0.4; newer packages are in packaging/devkitpro.
ifneq ($(shell $(PKGCONF) --atleast-version=2.8.0 SDL2_image && echo ok),ok)
$(warning SDL2_image is older than 2.8 - see packaging/devkitpro/README.md)
endif

# libcurl (+ mbedtls/zlib) for the network widgets (weather, chat).
CURL_CFLAGS := $(shell $(PKGCONF) --cflags libcurl)
CURL_LIBS   := $(shell $(PKGCONF) --libs libcurl)

# Trimmed static FFmpeg (H.264 decode + nvtegra NVDEC hwaccel) for video
# wallpapers. Built from source by scripts/build-ffmpeg.sh into its own
# prefix, same shape as $(DEVKITPRO)/AtmosphereLibs for sSystem - it is not a
# devkitPro portlib, so no pkg-config .pc file; flags are hand-rolled like
# the Lua/sol2 block below. See THIRDPARTY.md.
FFMPEG_PREFIX := $(DEVKITPRO)/FFmpegLibs
FFMPEG_CFLAGS := -I$(FFMPEG_PREFIX)/include
# Link order matters for static libs: avformat depends on avcodec depends on
# avutil (same reasoning as the curl/mbedtls/z ordering below).
FFMPEG_LIBS   := -lavformat -lavcodec -lavutil

#---------------------------------------------------------------------------------
# sMenu is a library applet: its exefs (main + main.npdm) lives in a folder on
# the SD card, which sSystem serves into an applet slot via ECS.
SDOUT  := ../../SdOut
OUTDIR := $(SDOUT)/slaunch/bin/sMenu

#---------------------------------------------------------------------------------
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := sMenu
APP_AUTHOR  := etonedemid
APP_VERSION := 1.4.1

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

# SL_HAS_NVTEGRA: only this build links the vendored averne/FFmpeg nvtegra
# fork (scripts/build-ffmpeg.sh); the AV_HWDEVICE_TYPE_NVTEGRA/AV_PIX_FMT_NVTEGRA
# enum members that fork adds are not preprocessor macros, so Video.cpp cannot
# test for them with #ifdef - this flag is the compile-time signal instead.
# The sim's native build (stock host FFmpeg, software decode only) must NOT
# define this.
CFLAGS    = -g -Wall -O2 -ffunction-sections -fvisibility=default $(ARCH) \
            $(INCLUDE) $(SDL_CFLAGS) $(CURL_CFLAGS) $(FFMPEG_CFLAGS) $(LUA_DEFINES) \
            -DSL_VERSION=\"$(APP_VERSION)\" -DSL_HAS_VIDEO=1 -DSL_HAS_NVTEGRA=1

CXXFLAGS = $(CFLAGS) -std=gnu++20 -fno-rtti -fno-exceptions \
           $(SOL_FLAGS) $(COMPAT_FLAGS)

ASFLAGS  := -g $(ARCH)

# ignore-in-shared-libs, NOT ignore-in-object-files: the latter lets a function
# that is declared but never defined link anyway, as an unresolved PLT stub full
# of zeroes. Calling one executes 0x00000000 and fatals with an Undefined
# Instruction at a PC inside .bss, which reads like memory corruption and is
# miserable to trace back to a missing definition. Our own objects must resolve.
LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(TARGET).map \
            -Wl,--unresolved-symbols=ignore-in-shared-libs

# Network libs first (curl -> mbedtls -> z), then FFmpeg (avformat -> avcodec
# -> avutil), then SDL (whose flags end with -lnx) so libnx resolves
# everything last.
LIBS     := $(CURL_LIBS) $(FFMPEG_LIBS) $(SDL_LIBS)

LIBDIRS  := $(PORTLIBS) $(LIBNX) $(FFMPEG_PREFIX)

#---------------------------------------------------------------------------------
# Lua is out-of-tree too, and lands outside build/ for the same reason the
# shared source did (see COMMON_OBJ below) - so its objects survived `make
# clean` and were silently reused. Its objects go under build/Lua/ instead.
LUA_SRC  := $(filter-out %/lua.c %/luac.c,$(wildcard ../../libs/Lua/*.c))
LUA_OBJ  := $(patsubst %.c,$(BUILD)/Lua/%.o,$(notdir $(LUA_SRC)))

CFILES   := $(foreach dir,$(filter-out ../../libs/Lua,$(SOURCES)),$(wildcard $(dir)/*.c))

CPPFILES := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.cpp))

# Sources shared with the other project get an object inside THIS project's
# build dir. The obvious $(BUILD)/%.o mapping does not: "../../libs/..." keeps
# its leading ../.., so build/../../libs/... resolves to projects/libs/... -
# the same path for sMenu and for sSystem. Both then compiled this file there,
# with their own flags (sysmodule specs here, libnx applet specs there), and
# each linked whichever object the other had written last. That is a menu
# object inside the daemon, or the reverse, decided by build order.
# Atmosphère's MTP server (libs/haze). Built as it is upstream: C++23 and
# Atmosphère's libvapours headers, from the same checkout sSystem builds
# libstratosphere from. Objects under build/haze/ for the reason given above.
ATMOSPHERE_DIR ?= /opt/atmosphere
HAZE_SRC   := $(wildcard ../../libs/haze/source/*.cpp)
HAZE_OBJ   := $(patsubst %.cpp,$(BUILD)/haze/%.o,$(notdir $(HAZE_SRC)))
HAZE_FLAGS := -std=gnu++23 -I$(ATMOSPHERE_DIR)/libraries/libvapours/include
# The menu's own wrapper includes haze's headers, so it needs them too.
$(BUILD)/source/sl/menu/usb/Mtp.o: CXXFLAGS += $(HAZE_FLAGS)

COMMON_OBJ := $(patsubst %.cpp,$(BUILD)/sCommon/%.o,$(notdir $(COMMON_SRC)))
SFILES   := $(foreach dir,$(SOURCES),$(wildcard $(dir)/*.s))
OFILES   := $(patsubst %.cpp,$(BUILD)/%.o,$(CPPFILES)) $(COMMON_OBJ) $(LUA_OBJ) $(HAZE_OBJ) \
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

$(BUILD)/Lua/%.o: ../../libs/Lua/%.c | $(BUILD)
	@mkdir -p $(dir $@)
	@$(CC) $(CFLAGS) -MMD -MP -c $< -o $@

$(BUILD)/haze/%.o: ../../libs/haze/source/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) $(HAZE_FLAGS) -MMD -MP -c $< -o $@

$(BUILD)/sCommon/%.o: ../../libs/sCommon/source/sl/os/%.cpp | $(BUILD)
	@mkdir -p $(dir $@)
	@$(CXX) $(CXXFLAGS) -MMD -MP -c $< -o $@

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