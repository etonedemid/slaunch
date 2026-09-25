# devkitPro packages

Newer SDL2 satellite libraries for the Switch, as PKGBUILDs in the layout of
[devkitPro/pacman-packages](https://github.com/devkitPro/pacman-packages), so
they can go upstream as they are. devkitPro ships SDL2 2.28.5 but SDL2_image
and SDL2_mixer 2.0.4 from 2018.

| Package | devkitPro | here | What changes |
|---|---|---|---|
| `switch-sdl2_image` | 2.0.4 | 2.8.12 | JPG and PNG decoded by the bundled stb_image (NEON), saved with miniz and tiny_jpeg. libjpeg and libpng are no longer linked. |
| `switch-sdl2_mixer` | 2.0.4 | 2.8.2 | MP3, FLAC and OGG decoded by the bundled minimp3, dr_flac and stb_vorbis. Still links libmodplug (MOD) and opusfile (Opus); mpg123, libFLAC and Tremor are no longer needed. |

Why sLaunch wants them: libjpeg reports a decode error by `longjmp`ing out of
the decoder, and on the console that path crashes the app, so one corrupt
JPEG took the whole menu down. stb_image returns an error instead. 2.8 also
brings `Mix_GetMusicPosition`, `Mix_MusicDuration` and six years of fixes.

Both keep the full 2.0.4 API, so existing code builds unchanged.

## Building and installing

Needs `dkp-toolchain-vars` (for `switchvars.sh`) and the packages each one
depends on:

```
sudo dkp-pacman -S --needed dkp-toolchain-vars switch-sdl2 switch-libwebp switch-libmodplug switch-opusfile
cd switch/SDL2_image && makepkg -f && cd ../SDL2_mixer && makepkg -f && cd ../..
sudo dkp-pacman -U switch/SDL2_image/switch-sdl2_image-*.pkg.tar.zst \
                   switch/SDL2_mixer/switch-sdl2_mixer-*.pkg.tar.zst
```

(`dkp-pacman` is `pacman` on Arch and other pacman-based hosts.) They replace
devkitPro's 2.0.4 packages; reinstalling those undoes it.
