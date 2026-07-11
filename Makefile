#---------------------------------------------------------------------------------
# sLaunch - Top-level Makefile
# Builds all components and produces the full SD card layout under SdOut/
#
# Requirements:
#   devkitPro + devkitA64 + libnx
#   Atmosphere AtmosphereLibs (for ldrShellAtmosphereRegisterExternalCode)
#   switch-curl, switch-mbedtls (for sInstaller)
#
# Usage:
#   make                    - build everything
#   make ssystem            - backend system applet only
#   make smenu              - text UI applet only
#   make sinstaller         - installer NRO only
#   make nxlink             - build installer + send via nxlink
#   make nxlink SWITCH_IP=x - same but to a specific IP
#   make clean              - remove all build artifacts
#   make package            - zip SdOut/ into slaunch.zip
#
# Install devkitPro packages:
#   dkp-pacman -S devkitA64 libnx switch-tools switch-curl switch-mbedtls
#---------------------------------------------------------------------------------

.PHONY: all ssystem smenu sinstaller nxlink clean package assets

all: ssystem smenu sinstaller assets
	@echo ""
	@echo "=== sLaunch build complete ==="
	@echo "SD card layout ready in SdOut/"
	@echo ""
	@echo "  SdOut/atmosphere/contents/0100000000001000/exefs/{main,main.npdm}  <- sSystem daemon (qlaunch)"
	@echo "  SdOut/slaunch/bin/sMenu/{main,main.npdm}                           <- sMenu SDL applet (via ECS)"
	@echo "  SdOut/slaunch/fonts/                                               <- bundled fonts"
	@echo "  SdOut/switch/sInstaller/sInstaller.nro                             <- installer homebrew"
	@echo ""

# Copy bundled assets (fonts + a themes folder for user wallpapers) into the
# SD layout so they land at sdmc:/slaunch/... on the device.
assets:
	@echo "--- Staging assets ---"
	@mkdir -p SdOut/slaunch/fonts SdOut/slaunch/themes SdOut/slaunch/bin/hbloader SdOut/slaunch/bin/hbloader_app SdOut/slaunch/widgets SdOut/slaunch/lang
	@cp -f assets/fonts/*.ttf SdOut/slaunch/fonts/ 2>/dev/null || true
	@cp -f assets/fonts/*.otf SdOut/slaunch/fonts/ 2>/dev/null || true
	@cp -f assets/fonts/LICENSE-OFL.txt assets/fonts/ATTRIBUTION.md SdOut/slaunch/fonts/ 2>/dev/null || true
	@# Default Lua widgets. example.lua is reference-only, so it is not shipped.
	@# All ship disabled by default (Theming > Widgets turns them on).
	@cp -f assets/widgets/weather.lua assets/widgets/auroracross.lua \
	       assets/widgets/clock.lua assets/widgets/calendar.lua \
	       assets/widgets/crypto.lua assets/widgets/countdown.lua \
	       assets/widgets/quote.lua SdOut/slaunch/widgets/ 2>/dev/null || true
	@# Locale template for translators (English is built in; no file = English).
	@cp -f assets/lang/*.txt SdOut/slaunch/lang/ 2>/dev/null || true
	@# Black/white icons for the system menu entries (List + Grid modes).
	@mkdir -p SdOut/slaunch/icons
	@cp -f assets/theming.png assets/controllers.png assets/album.png assets/user.png \
	       assets/browser.png assets/mii.png assets/settings.png assets/power.png \
	       assets/homebrewmenu.png SdOut/slaunch/icons/ 2>/dev/null || true
	@# nx-hbloader exefs served via ECS for the Homebrew menu (loads hbmenu.nro).
	@# Applet variant (application_type=2) runs homebrew as a library applet.
	@cp -f assets/hbloader/main assets/hbloader/main.npdm SdOut/slaunch/bin/hbloader/ 2>/dev/null || true
	@# Application variant: same loader, npdm with application_type=1 so the donor
	@# process is a real application (am grants an application proxy -> full RAM).
	@cp -f assets/hbloader/main SdOut/slaunch/bin/hbloader_app/main 2>/dev/null || true
	@cp -f assets/hbloader/main_app.npdm SdOut/slaunch/bin/hbloader_app/main.npdm 2>/dev/null || true
	@# Background music tracks + UI sound effects (welcome / page turn).
	@mkdir -p SdOut/slaunch/music SdOut/slaunch/sounds
	@cp -f assets/music/*.mp3 assets/music/*.ogg assets/music/*.flac SdOut/slaunch/music/ 2>/dev/null || true
	@cp -f assets/UI/*.wav SdOut/slaunch/sounds/ 2>/dev/null || true

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
