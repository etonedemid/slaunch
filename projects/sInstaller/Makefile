#---------------------------------------------------------------------------------
# sInstaller Makefile — builds the Switch installer NRO
# Run via Homebrew Menu or send via nxlink: make nxlink
#---------------------------------------------------------------------------------

TARGET   := sInstaller
BUILD    := build
SOURCES  := source
INCLUDES := include

SDOUT    := ../../SdOut
OUTDIR   := $(SDOUT)/switch/sInstaller

#---------------------------------------------------------------------------------
include $(DEVKITPRO)/libnx/switch_rules

APP_TITLE   := sLaunch Installer
APP_AUTHOR  := sLaunch
APP_VERSION := 0.1.0
APP_ICON    := ../../assets/icon.jpg

#---------------------------------------------------------------------------------
ARCH     := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE
CFLAGS    = -g -Wall -O2 -ffunction-sections $(ARCH) \
            $(INCLUDE) -D__SWITCH__ \
            -DSL_VERSION=\"$(APP_VERSION)\" \
            -DGH_OWNER=\"etonedemid\" \
            -DGH_REPO=\"slaunch\"

CXXFLAGS  = $(CFLAGS) -std=c++20 -fno-rtti -fno-exceptions

LDFLAGS  := -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) \
            -Wl,-Map,$(BUILD)/$(TARGET).map

LIBS     := -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lnx -lz

LIBDIRS  := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
CPPFILES := $(wildcard $(SOURCES)/*.cpp)
OFILES   := $(patsubst %.cpp,$(BUILD)/%.o,$(notdir $(CPPFILES)))

INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
            -I$(LIBNX)/include -I$(PORTLIBS)/include

VPATH    := $(SOURCES)

#---------------------------------------------------------------------------------
.PHONY: all clean nxlink nxlink-server

all: $(OUTDIR)/$(TARGET).nro

# Build the NRO
$(OUTDIR)/$(TARGET).nro: $(BUILD)/$(TARGET).nacp $(BUILD)/$(TARGET).elf | $(OUTDIR)
	@elf2nro $(BUILD)/$(TARGET).elf $@ --nacp=$(BUILD)/$(TARGET).nacp \
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
	@rm -rf $(BUILD) $(OUTDIR)/$(TARGET).nro

# ---------------------------------------------------------------------------
# nxlink — send the NRO to a Switch over the network
# Usage: make nxlink SWITCH_IP=192.168.1.x
#        (if SWITCH_IP not set, nxlink broadcasts to find the console)
nxlink: all
	@echo "Sending $(TARGET).nro via nxlink..."
	@nxlink -s $(OUTDIR)/$(TARGET).nro $(if $(SWITCH_IP),-a $(SWITCH_IP),)

# nxlink-server: send NRO AND stay connected to receive stdout logs
nxlink-server: all
	@echo "Sending $(TARGET).nro via nxlink (with log server)..."
	@nxlink -s $(OUTDIR)/$(TARGET).nro $(if $(SWITCH_IP),-a $(SWITCH_IP),)

-include $(OFILES:.o=.d)
