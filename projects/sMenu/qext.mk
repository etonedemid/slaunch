# sMenu as a qlaunch-ext menu: a dynamically loaded module (.dnro) that the
# framework's loader runs in the HOME Menu process, instead of a library applet
# plus the sSystem daemon. Same sources as the applet build (SL_QEXT picks the
# differences). libc, libstdc++ and libnx come from the loader, SDL3 and FFmpeg
# from their own modules; see scripts/build-qext.sh for the whole chain.
#
#   make -f qext.mk QEXT_DIR=~/qlaunch-ext

QEXT_DIR    ?= $(HOME)/qlaunch-ext
SDL3_PREFIX ?= $(HOME)/sdl3-switch/stage
ATMOSPHERE_DIR ?= /opt/atmosphere
DEVKITPRO   ?= /opt/devkitpro
# The loader needs libnx master, like sSystem (see its Makefile); same headers here.
LIBNX   ?= $(HOME)/Desktop/Projects/switch/libnx-master-install/opt/devkitpro/libnx
DLINK   := $(QEXT_DIR)/extern/libnx-dlink
SDLDIR  := $(QEXT_DIR)/extern/SDL-dlink
FFDIR   := $(QEXT_DIR)/extern/FFmpeg-dlink/install
PORTLIBS := $(DEVKITPRO)/portlibs/switch
A64     := $(DEVKITPRO)/devkitA64/bin/aarch64-none-elf-
BUILD   := build-qext
OUT     := $(BUILD)/sMenu

ARCH := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC
# No thread_local anywhere: the module loader sets up no TLS block, so a write
# to one faults (it took the menu down in sol2's first state, on hardware).
# SDL-dlink's headers first; the satellite libraries' from the stage prefix.
FLAGS := -idirafter ../../libs/discord -g -O2 -Wall -ffunction-sections -fdata-sections -fvisibility=default $(ARCH) \
    -D__SWITCH__ -DSL_QEXT=1 -DSL_LIBNX_MASTER -DSL_VERSION=\"2.1.4\" -DSL_HAS_VIDEO=1 -DSL_HAS_NVTEGRA=1 \
    -D_longjmp=longjmp -D_setjmp=setjmp -DLUAI_MAXSTACK=1000000 -DLUA_32BITS -DLUAI_FUNC=extern \
    -I$(SDLDIR)/include -I$(SDL3_PREFIX)/include -I$(FFDIR)/include \
    -I$(LIBNX)/include -I$(PORTLIBS)/include -I$(SDLDIR)/extern/neko3d/include -I$(BUILD) \
    -Iinclude -I../../libs/sCommon/include -I../sSystem/include -I../../libs/Lua \
    -I../../libs/sol2/include -I../../libs/haze/include
CFLAGS   := $(FLAGS)
CXXFLAGS := $(FLAGS) -std=gnu++20 -fno-rtti -fno-exceptions \
    -DSOL_ALL_SAFETIES_ON=0 -DSOL_EXCEPTIONS_SAFE_PROPAGATION=0 -DSOL_NO_EXCEPTIONS \
    -DSOL_NO_PLAIN=1 -Dsol_lua_newstate_args=2 -DSOL_NO_THREAD_LOCAL=1
HAZE_FLAGS := -std=gnu++23 -I$(ATMOSPHERE_DIR)/libraries/libvapours/include

HAZE_SRCS := $(wildcard ../../libs/haze/source/*.cpp)
# HomeNX's Discord client. It has try/catch; nothing in it is meant to throw across
# the module's edge.
DISCORD_SRCS := $(wildcard ../../libs/discord/*/*.cpp)
DISCORD_FLAGS := -fexceptions -I../../libs/discord

# MessageHandler reads the daemon's queue; Crash overrides libnx's exception
# handler, which only works linked into the process image (the loader's).
CPPS := $(filter-out source/sl/menu/smi/MessageHandler.cpp source/sl/menu/dbg/Crash.cpp, \
          source/main.cpp $(wildcard source/sl/menu/*/*.cpp)) \
        ../../libs/sCommon/source/sl/os/Applications.cpp \
        ../sSystem/source/sl/sys/pwr/Power.cpp \
        ../sSystem/source/sl/sys/hb/LaunchQueue.cpp \
        $(HAZE_SRCS) $(DISCORD_SRCS)
CS   := $(filter-out %/lua.c %/luac.c,$(wildcard ../../libs/Lua/*.c)) \
        ../../libs/discord/third_party/yyjson.c \
        $(QEXT_DIR)/menu/stub/stub.c
OBJS := $(patsubst %,$(BUILD)/o/%.o,$(subst ../,up/,$(subst $(QEXT_DIR)/,qext/,$(CPPS) $(CS)))) \
        $(BUILD)/o/crt0.o

LIBS := -L$(SDL3_PREFIX)/lib -L$(PORTLIBS)/lib \
    -lSDL3_ttf -lharfbuzz -lfreetype -lbz2 -lSDL3_image -lwebpdemux -lwebpmux -lwebp -lsharpyuv -lpng16 -lSDL3_mixer \
    -lopusfile -lopus -logg -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz -lgcc

$(OUT).dnro: $(OUT).elf
	python3 $(DLINK)/tools/elf2dnro.py $< $@

# -z nopack-relative-relocs: the module loader applies RELA, not RELR.
$(OUT).elf: $(OBJS)
	$(A64)g++ -specs=$(DLINK)/tools/module.specs -g $(ARCH) -shared -nodefaultlibs \
	    -Wl,--export-dynamic -Wl,-Bsymbolic -Wl,--gc-sections -Wl,-z,nopack-relative-relocs \
	    -Wl,-Map,$(OUT).map -o $@ $(OBJS) $(LIBS)
	$(A64)nm -D -u $@ | awk '{print $$NF}' | sort -u > $(OUT).undef

define compile_rule
$(BUILD)/o/$(subst ../,up/,$(subst $(QEXT_DIR)/,qext/,$(1))).o: $(1)
	@mkdir -p $$(dir $$@)
	@echo $(notdir $(1))
	@$(2) $(3) -MMD -MP -c $$< -o $$@
endef
$(foreach f,$(filter-out $(HAZE_SRCS) $(DISCORD_SRCS) source/sl/menu/usb/Mtp.cpp,$(CPPS)),$(eval $(call compile_rule,$(f),$(A64)g++,$(CXXFLAGS))))
$(foreach f,$(DISCORD_SRCS),$(eval $(call compile_rule,$(f),$(A64)g++,$(CXXFLAGS) $(DISCORD_FLAGS))))
$(foreach f,$(HAZE_SRCS) source/sl/menu/usb/Mtp.cpp,$(eval $(call compile_rule,$(f),$(A64)g++,$(CXXFLAGS) $(HAZE_FLAGS))))
$(foreach f,$(CS),$(eval $(call compile_rule,$(f),$(A64)gcc,$(CFLAGS))))

$(BUILD)/o/crt0.o: $(QEXT_DIR)/menu/stub/crt0.s
	@mkdir -p $(dir $@)
	@$(A64)gcc -g $(ARCH) -c $< -o $@

# The effects' shaders, compiled for deko3d from the GLSL literals in these sources.
FX_SOURCES := source/sl/menu/gfx/Gfx.cpp source/sl/menu/ui/Menu_Internal.hpp source/sl/menu/ui/Menu.cpp
$(BUILD)/fx_dksh.h: scripts/fx-spirv.py $(FX_SOURCES)
	@mkdir -p $(BUILD)
	@DEVKITPRO=$(DEVKITPRO) python3 scripts/fx-spirv.py --dksh $@ $(FX_SOURCES)
$(BUILD)/o/source/sl/menu/gfx/Gfx.cpp.o: $(BUILD)/fx_dksh.h

-include $(OBJS:.o=.d)

clean:
	rm -rf $(BUILD)
.PHONY: clean
