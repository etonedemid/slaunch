#---------------------------------------------------------------------------------
# sLaunch — Top-level Makefile
# Builds all components and produces the full SD card layout under SdOut/
#
# Requirements:
#   devkitPro + devkitA64 + libnx
#   Atmosphere AtmosphereLibs (for ldrShellAtmosphereRegisterExternalCode)
#   switch-curl, switch-mbedtls (for sInstaller)
#
# Usage:
#   make                    — build everything
#   make ssystem            — backend system applet only
#   make smenu              — text UI applet only
#   make sinstaller         — installer NRO only
#   make nxlink             — build installer + send via nxlink
#   make nxlink SWITCH_IP=x — same but to a specific IP
#   make clean              — remove all build artifacts
#   make package            — zip SdOut/ into slaunch.zip
#
# Install devkitPro packages:
#   dkp-pacman -S devkitA64 libnx switch-tools switch-curl switch-mbedtls
#---------------------------------------------------------------------------------

.PHONY: all ssystem smenu sinstaller nxlink clean package

all: ssystem smenu sinstaller
	@echo ""
	@echo "=== sLaunch build complete ==="
	@echo "SD card layout ready in SdOut/"
	@echo ""
	@echo "  SdOut/atmosphere/contents/0100000000001000/exefs/main  <- sSystem (qlaunch replacement)"
	@echo "  SdOut/slaunch/bin/sMenu/main                           <- sMenu text UI"
	@echo "  SdOut/switch/sInstaller/sInstaller.nro                 <- installer homebrew"
	@echo ""

ssystem:
	@echo "--- Building sSystem ---"
	@$(MAKE) -C projects/sSystem

smenu:
	@echo "--- Building sMenu ---"
	@$(MAKE) -C projects/sMenu

sinstaller:
	@echo "--- Building sInstaller ---"
	@$(MAKE) -C projects/sInstaller

# Send the installer NRO to a running Switch via nxlink.
# The Switch must be on the same network, running the Homebrew Menu
# (or any NRO that calls nxlinkInitialize / accepts nxlink connections).
#
# Usage:
#   make nxlink                         # auto-discover Switch via broadcast
#   make nxlink SWITCH_IP=192.168.1.42  # target a specific IP
nxlink: sinstaller
	@$(MAKE) -C projects/sInstaller nxlink $(if $(SWITCH_IP),SWITCH_IP=$(SWITCH_IP),)

clean:
	@$(MAKE) -C projects/sSystem    clean
	@$(MAKE) -C projects/sMenu      clean
	@$(MAKE) -C projects/sInstaller clean
	@rm -rf SdOut/
	@echo "Cleaned."

package: all
	@echo "--- Packaging release ---"
	@rm -f slaunch.zip
	@cd SdOut && zip -r ../slaunch.zip .
	@echo "Created slaunch.zip"
	@ls -lh slaunch.zip
