# Desktop simulator for the sLaunch menu.
#
# Two targets from one set of sources:
#
#   make          native Linux binary, shown through WSLg
#   make win      Windows .exe, cross-compiled with mingw-w64
#
# The Windows build differs only by a force-included compatibility header; see
# include/sim_win_compat.h for what it does and why the menu is untouched.
#
#   make -C projects/sMenu/sim
#   projects/sMenu/sim/sync-sd.sh /mnt/e      # transplant the card
#   cd projects/sMenu/sim && ./slaunch-sim
#
# See README.md.

SIM_DIR   := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
MENU_DIR  := $(abspath $(SIM_DIR)..)
ROOT_DIR  := $(abspath $(MENU_DIR)/../..)

TARGET    := slaunch-sim
BUILD     := build

# The menu sources that make sense on a desktop. main.cpp is replaced by
# src/main_sim.cpp, and MessageHandler.cpp is left out: it talks to the sSystem
# daemon over applet storages, which has no desktop equivalent.
MENU_SRCS := \
	$(MENU_DIR)/source/sl/menu/ui/Menu.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Touch.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_List.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Grid.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Cover.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Shelf.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Xmb.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Flow.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Deck.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Files.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Menu_Screens.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Theme.cpp \
	$(MENU_DIR)/source/sl/menu/ui/Locale.cpp \
	$(MENU_DIR)/source/sl/menu/gfx/Gfx.cpp \
	$(MENU_DIR)/source/sl/menu/gfx/IconCache.cpp \
	$(MENU_DIR)/source/sl/menu/gfx/Video.cpp \
	$(MENU_DIR)/source/sl/menu/audio/Music.cpp \
	$(MENU_DIR)/source/sl/menu/audio/Sound.cpp \
	$(MENU_DIR)/source/sl/menu/hb/Homebrew.cpp \
	$(MENU_DIR)/source/sl/menu/hb/Shortcuts.cpp \
	$(MENU_DIR)/source/sl/menu/net/Http.cpp \
	$(MENU_DIR)/source/sl/menu/net/ContentFilter.cpp \
	$(MENU_DIR)/source/sl/menu/news/News.cpp \
	$(MENU_DIR)/source/sl/menu/play/PlayStats.cpp \
	$(MENU_DIR)/source/sl/menu/widgets/Widgets.cpp \
	$(MENU_DIR)/source/sl/menu/cfg/UserCfg.cpp \
	$(MENU_DIR)/source/sl/menu/dbg/Debug.cpp

SIM_SRCS  := \
	$(SIM_DIR)src/main_sim.cpp \
	$(SIM_DIR)src/switch_stub.cpp \
	$(SIM_DIR)src/win_path.cpp

# The Lua library, minus its two standalone front ends: lua.c and luac.c each
# define main(), which collides with the simulator's.
LUA_SRCS  := $(filter-out %/lua.c %/luac.c, $(wildcard $(ROOT_DIR)/libs/Lua/*.c))

SRCS      := $(MENU_SRCS) $(SIM_SRCS)
OBJS      := $(patsubst %,$(BUILD)/%.o,$(notdir $(SRCS))) \
             $(patsubst %,$(BUILD)/lua/%.o,$(notdir $(LUA_SRCS)))

VPATH     := $(sort $(dir $(SRCS))) $(ROOT_DIR)/libs/Lua

INCLUDES  := -I$(SIM_DIR)include \
             -I$(MENU_DIR)/include \
             -I$(ROOT_DIR)/libs/sCommon/include \
             -I$(ROOT_DIR)/libs/Lua \
             -I$(ROOT_DIR)/libs/sol2/include

# libavformat/libavcodec/libavutil: the host's own FFmpeg (software H.264
# decode - there is no Tegra/NVDEC on a desktop). Same VideoPlayer source as
# the Switch build; only the hwaccel that comes back from avcodec_open2()
# differs. See THIRDPARTY.md.
PKGS      := sdl2 SDL2_image SDL2_ttf SDL2_mixer libcurl \
             libavformat libavcodec libavutil
PKGCFLAGS := $(shell pkg-config --cflags $(PKGS))
PKGLIBS   := $(shell pkg-config --libs $(PKGS))

# -Wno-multichar: the menu uses multi-character literals for a couple of tags,
# which devkitA64 accepts silently and host g++ warns about.
WARN      := -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers \
             -Wno-multichar

# Read out of the console build so the simulator never reports a version that
# build does not.
APP_VERSION := $(shell sed -n 's/^APP_VERSION[[:space:]]*:=[[:space:]]*//p' $(MENU_DIR)/Makefile)
DEFINES   := -DSL_VERSION=\"$(APP_VERSION)\" -DSL_SIMULATOR=1

# -MMD -MP: without header dependencies, editing a header rebuilt nothing and
# the next run was a binary built from mismatched objects.
# SL_HAS_VIDEO: native build links the host's FFmpeg (PKGS above), so
# Video.cpp compiles its real implementation here. The Windows cross-build
# below does not stage FFmpeg for MinGW, so it leaves this unset and
# Video.cpp compiles to a stub there instead (ponytail: no MinGW FFmpeg
# staging; add via get-win-deps.sh if a Windows video-wallpaper preview is
# ever needed).
CXXFLAGS  := -std=gnu++20 -O1 -g $(WARN) $(INCLUDES) $(PKGCFLAGS) $(DEFINES) \
             -DSL_HAS_VIDEO=1 -MMD -MP
CFLAGS    := -std=gnu11 -O2 -g -w -I$(ROOT_DIR)/libs/Lua
LDFLAGS   := $(PKGLIBS) -lpthread -lm
# Real SDL 2.28, when built into this prefix (see README, "GPU effects"). Arch
# ships sdl2-compat on SDL3, whose SDL_GL_BindTexture binds nothing, so the
# GPU texture paths (Flow's 3D boxes, cards) would fall back to their CPU
# versions. The console runs real SDL 2.28.5; a runpath to the same version
# here makes the simulator draw what the console draws.
SIM_SDL2  := $(HOME)/.local/share/slaunch-sim-sdl2/lib
ifneq ($(wildcard $(SIM_SDL2)/libSDL2-2.0.so.0),)
LDFLAGS   += -Wl,--enable-new-dtags,-rpath,$(SIM_SDL2)
endif

# ---------------------------------------------------------------------------
# Windows cross-build.
#
# SDL2 for MinGW is not in the Arch repositories, so scripts/get-win-deps.sh
# stages the official prebuilt development packages into $(WIN_DEPS). Those are
# headers, import libraries and DLLs; nothing is compiled from them.
WIN_DEPS   ?= $(HOME)/.slaunch-windeps/prefix
WIN_CXX    := x86_64-w64-mingw32-g++
WIN_CC     := x86_64-w64-mingw32-gcc
WIN_BUILD  := build-win
WIN_TARGET := slaunch-sim.exe
WIN_DIST   := dist-win

# The same source list as the Linux build, Http.cpp included: libcurl for MinGW
# is staged alongside SDL2, so the Windows build compiles the shipping
# networking code rather than a substitute for it.
WIN_SRCS := $(SRCS)
WIN_OBJS := $(patsubst %,$(WIN_BUILD)/%.o,$(notdir $(WIN_SRCS))) \
            $(patsubst %,$(WIN_BUILD)/lua/%.o,$(notdir $(LUA_SRCS)))

# include-win first: it holds Windows-only shims for POSIX headers MinGW does
# not ship (sys/socket.h and friends), and must be found ahead of anything else.
WIN_INCLUDES := -I$(SIM_DIR)include-win \
                -I$(SIM_DIR)include \
                -I$(MENU_DIR)/include \
                -I$(ROOT_DIR)/libs/sCommon/include \
                -I$(ROOT_DIR)/libs/Lua \
                -I$(ROOT_DIR)/libs/sol2/include \
                -I$(WIN_DEPS)/include

# The compatibility header is force-included ahead of everything else, which is
# the only way its macros can be in place before the menu's own includes are
# read. -mconsole keeps a console attached so the diagnostics stay visible.
WIN_CXXFLAGS := -std=gnu++20 -O1 $(WARN) $(WIN_INCLUDES) $(DEFINES) \
                -include sim_win_compat.h
WIN_CFLAGS   := -std=gnu11 -O2 -w -I$(ROOT_DIR)/libs/Lua
# -lws2_32 for the chat widget's sockets.
#
# -static-libgcc/-libstdc++ keep the compiler runtime inside the exe, but they do
# not cover winpthread, which std::thread pulls in - so libwinpthread-1.dll is
# copied next to the exe by the rule below. Without it Windows refuses to start
# the program at all, with 0xc000007b. Forcing it static with -Bstatic does not
# work here: gcc's own spec names the import library first and wins.
# --wrap=remove redirects the C remove() to __wrap_remove in src/win_path.cpp.
# It is done here rather than with a macro because the name is far too common to
# wrap in the preprocessor - see the note in include/sim_win_compat.h.
WIN_LDFLAGS  := -L$(WIN_DEPS)/lib -lmingw32 -lSDL2main -lSDL2 \
                -lSDL2_image -lSDL2_ttf -lSDL2_mixer -lcurl -lws2_32 \
                -Wl,--wrap=remove \
                -static-libgcc -static-libstdc++ -mconsole

.PHONY: test all clean run win win-clean
all: $(TARGET)

$(TARGET): $(OBJS)
	@echo "  LD   $@"
	@$(CXX) $(OBJS) $(LDFLAGS) -o $@

$(BUILD)/%.cpp.o: %.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  $(notdir $<)"
	@$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/lua/%.c.o: %.c
	@mkdir -p $(dir $@)
	@echo "  CC   $(notdir $<)"
	@$(CC) $(CFLAGS) -c $< -o $@

win: $(WIN_DIST)/$(WIN_TARGET)

$(WIN_DIST)/$(WIN_TARGET): $(WIN_OBJS)
	@mkdir -p $(WIN_DIST)
	@echo "  LD   $(WIN_TARGET)"
	@$(WIN_CXX) $(WIN_OBJS) $(WIN_LDFLAGS) -o $@
	@cp -f $(WIN_DEPS)/bin/*.dll $(WIN_DIST)/
	@cp -f /usr/x86_64-w64-mingw32/bin/libwinpthread-1.dll $(WIN_DIST)/
	@echo "  ->   $(WIN_DIST)/"

# Staged on first use. An order-only prerequisite, so it is fetched before any
# object is compiled but never triggers a rebuild afterwards.
WIN_DEPS_STAMP := $(WIN_DEPS)/include/SDL2/SDL.h

$(WIN_DEPS_STAMP):
	@echo "  DEPS staging SDL2 for MinGW into $(WIN_DEPS)"
	@sh $(SIM_DIR)scripts/get-win-deps.sh $(abspath $(WIN_DEPS)/..)

$(WIN_BUILD)/%.cpp.o: %.cpp | $(WIN_DEPS_STAMP)
	@mkdir -p $(dir $@)
	@echo "  CXX  $(notdir $<)  [win]"
	@$(WIN_CXX) $(WIN_CXXFLAGS) -c $< -o $@

$(WIN_BUILD)/lua/%.c.o: %.c | $(WIN_DEPS_STAMP)
	@mkdir -p $(dir $@)
	@echo "  CC   $(notdir $<)  [win]"
	@$(WIN_CC) $(WIN_CFLAGS) -c $< -o $@

win-clean:
	@rm -rf $(WIN_BUILD) $(WIN_DIST)

run: $(TARGET)
	@./$(TARGET)

# Self-check for the launcher-shortcut scan. Writes its own fixtures into a
# temp dir, so it needs no SD layout and nothing from the simulator but its
# switch.h - hence its own compile rather than a target built out of $(OBJS).
test: $(BUILD)/test_shortcuts $(BUILD)/test_letterjump $(BUILD)/test_search $(BUILD)/test_steammatch
	@./$(BUILD)/test_steammatch
	@./$(BUILD)/test_shortcuts
	@./$(BUILD)/test_letterjump
	@./$(BUILD)/test_search

$(BUILD)/test_steammatch: $(SIM_DIR)src/test_steammatch.cpp $(MENU_DIR)/source/sl/menu/net/Http.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  test_steammatch"
	@$(CXX) -std=c++17 -g -I$(SIM_DIR)include -I$(MENU_DIR)/include $^ -lcurl -o $@

$(BUILD)/test_search: $(SIM_DIR)src/test_search.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  test_search"
	@$(CXX) -std=c++17 -g $< -o $@

$(BUILD)/test_letterjump: $(SIM_DIR)src/test_letterjump.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  test_letterjump"
	@$(CXX) -std=c++17 -g $< -o $@

$(BUILD)/test_shortcuts: $(SIM_DIR)src/test_shortcuts.cpp \
                         $(MENU_DIR)/source/sl/menu/hb/Shortcuts.cpp \
                         $(MENU_DIR)/source/sl/menu/cfg/UserCfg.cpp
	@mkdir -p $(dir $@)
	@echo "  CXX  test_shortcuts"
	@$(CXX) -std=c++17 -g -I$(SIM_DIR)include -I$(MENU_DIR)/include \
	        -I$(ROOT_DIR)/libs/sCommon/include $^ -o $@

clean:
	@rm -rf $(BUILD) $(TARGET) $(WIN_BUILD) $(WIN_DIST)

-include $(OBJS:.o=.d)
-include $(WIN_OBJS:.o=.d)
