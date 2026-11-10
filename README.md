<img width="64" height="64" alt="icon" src="https://github.com/user-attachments/assets/0a70ab24-5098-4e7b-b6ce-dba046e9230e"/>  for the Nintendo Switch

[Discord](https://discord.gg/dv28MgtaNn) 

<img width="1280" height="720" alt="2026092401121200-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/fc5fcfc0-1348-454c-a057-2459c590818a" />
<img width="1280" height="720" alt="2026101522331400-A082AE4E5DA891D87084ACEACFDFF4A9" src="https://github.com/user-attachments/assets/25b4fedb-23fb-4c87-9cf6-0260a3c94ac2" />


## Support

If you'd like to support development:

- USDT (TRC20): `TCJ4HMJF3JHQG66khYTj98Em7v74j3cry9`
- Bitcoin: `bc1qaeypp2m5h74alrmhyu4zvzf3yyf58k6mcw7868`
- Monero: `45npN9u6zMFE7xMxfszfWBX2jdDdtZaogcGex4s5D7QDNsGPtbXjtjtXpbx9cnxLaKXC1KzGgckN2fMwrYrw5yqPTG2KiPj`

## Features

* **Theming & Customization:** Supports 5 built-in themes, custom user wallpapers, icon packs, custom fonts, and several UI modes. Includes layout tuning and customizable tile sizes/colors.
* **Video Wallpapers:** Supports looped, silent `.mp4` video backgrounds utilizing the Switch's native NVDEC hardware decoder.
* **Interactive Widgets:** Lua-based home screen widgets that can be dragged and repositioned via touch.
* **Background Audio:** Native support for background music playback (MP3/OGG/FLAC) and custom UI sound effects.
* **Per-Account Settings:** Fully independent profiles. Two users on the same console can have completely different themes, UI layouts, pinned homebrew, favorites, and hidden entries.
* **USB File Transfer (MTP):** Connect to a PC via USB while the menu is open to directly copy files to/from the SD card. Disconnects automatically when launching a game or app.
* **Content Filtering:** Opt-in adult and extreme-violence filter for downloaded game covers and news updates based on ratings (ESRB/PEGI/USK) and Steam tags.
* **System Localization:** Fully translatable UI using custom `.txt` files. Automatically reads the console's shared font for native CJK (Chinese, Japanese, Korean) rendering, with an included Noto Sans CJK font for fallback.
* **Diagnostic Logging:** Writes detailed boot, ECS, and daemon logs directly to the SD card to troubleshoot bring-up issues without a debugger.

## Blades skins

The Blades layout has its own built-in look. It can also take pictures from an
Xbox 360 dashboard package you have. sLaunch ships none of that art; it only
reads files you copied to the card.

1. Copy the dashboard's resource packages, `res.xzp` and `shrdres.xzp`, off
   your Xbox 360 (a modded console's file manager or FTP can do it), or out
   of a dashboard folder you already have on your PC.
2. Copy them to the Switch SD card, into `sdmc:/slaunch/skins/import/blades/`.
3. Switch to Blades in Theming > UI mode. sLaunch builds a skin called
   `dashboard` from the packages, selects it, and renames them to `.xzp.done`.
   To import again, put fresh copies in the folder and restart the menu.

The import takes the button glyphs, list panel, highlight and detail panel.
A community Blades skin package (one `.xzp` holding a `fond1.png` frame) can go
in `import/blades/` the same way: sLaunch cuts one blade tab out of its frame,
wipes the name off it, and uses it for every blade. That skin is named after
the file and selected at once.
Sounds and fonts are not converted; add `sounds/*.wav` or `font.ttf` to
`sdmc:/slaunch/skins/blades/<skin>/` yourself. Every file a skin can hold
is listed in `sdmc:/slaunch/skins/README.txt`. To go back to the built-in
look, delete the skin's folder.

## Build Instructions

1. **Install devkitPro and Dependencies:**
On Linux/WSL2, import the devkitPro keys and install the necessary Switch libraries:

```bash
sudo pacman-key --recv-keys BC26F752D25B92CE272E0F44F7FD5492264BB9D0 --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign-key BC26F752D25B92CE272E0F44F7FD5492264BB9D0
sudo pacman -U https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.xz

```

Ensure `dkp-libs` and `dkp-linux` are added to your `/etc/pacman.conf`, then install:

```bash
sudo pacman -Sy switch-dev switch-sdl2 switch-sdl2_ttf switch-sdl2_image switch-sdl2_mixer switch-curl switch-mbedtls

```

*(Note: Using SDL2_image 2.8 and SDL2_mixer 2.8 instead of devkitPro's default 2.0.4 is recommended to prevent JPEG decoding crashes).*


2. **Configure Environment Variables:**
Export the required devkitPro paths so the Makefiles can locate the compiler:

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITA64/bin:$PATH

```


3. **Build libnx master and libstratosphere:**
Atmosphere 1.12.0 (the release that supports firmware 23.0.x) needs a newer libnx than the packaged 4.12.0, so libnx master is built into a folder of its own for the daemon; the menu keeps the packaged libnx. Built against older libraries, the daemon does not start on 23.0.x.

```bash
git clone --depth 50 https://github.com/switchbrew/libnx ~/libnx-master
make -C ~/libnx-master/nx -j$(nproc)
make -C ~/libnx-master/nx install DESTDIR=$HOME/libnx-master-install
export SL_LIBNX=$HOME/libnx-master-install/opt/devkitpro/libnx

git clone --depth=1 --branch 1.12.0 https://github.com/Atmosphere-NX/Atmosphere /opt/atmosphere
make -C /opt/atmosphere/libraries/libstratosphere -j$(nproc) LIBNX=$SL_LIBNX

mkdir -p $DEVKITPRO/AtmosphereLibs/include $DEVKITPRO/AtmosphereLibs/lib
cp -r /opt/atmosphere/libraries/libstratosphere/include/. $DEVKITPRO/AtmosphereLibs/include/
cp -r /opt/atmosphere/libraries/libvapours/include/.      $DEVKITPRO/AtmosphereLibs/include/
find /opt/atmosphere/libraries/libstratosphere -name '*.a' -exec cp {} $DEVKITPRO/AtmosphereLibs/lib/ \;
```

The daemon's Makefile looks for that libnx in `~/Desktop/Projects/switch/libnx-master-install` by default; point it elsewhere with `make LIBNX=$SL_LIBNX`.


4. **Build sLaunch:**
Run `make` in the sLaunch root directory. This compiles the daemon, applet, and bundles all assets into the `SdOut/` folder.

* Use `make package` to create a deployable zip file.
* Individual targets available: `make ssystem`, `make smenu`, `make sinstaller`, `make hbloader`, `make assets`.


## Credits

* **uLaunch (Xortroll & contributors):** Architecture, applet/daemon model, ECS content-serving, and NPDMs (GPLv2).
* **nx-hbloader (switchbrew):** Foundation for the bundled homebrew loader fork (ISC).
* **Atmosphere (haze):** USB file transfer protocol (GPLv2).
* **MeepCat55:** Creator of the bundled `Minimal` icon pack.
* **Xelu:** Free Controller and Key Prompts used for UI buttons (CC0).
* **averne / FFmpeg (nvtegra branch) & SwitchWave:** NVDEC hardware-acceleration backend used for video wallpaper decoding (GPLv2+).
