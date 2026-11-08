# Class of '09 — Nintendo Switch homebrew port

A native Nintendo Switch homebrew port of the *Class of '09* runtime.

This is not a Ren'Py port. The game script is interpreted from the same
`script.json` used by the PS5 build, with SDL2 handling video, input, audio,
images and fonts. The WebM cutscenes are decoded directly from the original
game files with libvpx (VP8) and libvorbis — there is no frame-sequence
conversion step and no `assets/videos` folder.

Unofficial fan project. Not affiliated with the game's developers or
publishers, Nintendo, or Sony.

## Features

- WebM playback on hardware: libvpx/libvorbis decoding, 720p upload path,
  threaded decode so menus and gameplay stay smooth.
- Dialogue voice decoded from the original per-scene MP3s (minimp3) and played
  with frame-aligned PCM, so lines are clean and stay in sync when skipping.
- Switch-native controls: physical **A** confirms, **B** goes back, and the
  left stick acts as the D-pad.
- Saves, ending tracker records and screenshots on the SD card.

## SD card layout

```
sdmc:/switch/class09/class09.nro
sdmc:/switch/class09/assets/script.json
sdmc:/switch/class09/assets/game/...      (your game files, incl. the .webm videos)
```

The `assets` folder is the same one used by the PS5 build and can be copied
straight across. Saves, ending records and screenshots are written to
`sdmc:/switch/class09/saves/`; the console creates the folder on first run.

## Controls

| Button | Action |
| --- | --- |
| A | advance / confirm |
| B | step back one line / back |
| Y | ending tracker (main menu) |
| D-pad / left stick | navigate |
| + or − | pause menu |
| HOME | close the app |

Ending videos play to the end; the logo and the New Game intro can be skipped
with **A** after a short delay.

## Building

Requires devkitPro with the `switch-dev` group — devkitA64, libnx,
switch-sdl2, switch-sdl2_image, switch-sdl2_mixer, switch-sdl2_ttf — plus
switch-libvpx and switch-libvorbis.

```sh
export DEVKITPRO=/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH
make -j8
```

The output is `class09.nro`. Build from a path without spaces: the devkitPro
makefiles do not quote paths.

SDL2 is linked statically, so the `.nro` is self-contained apart from the
SD-card assets.

## Repository layout

```
Makefile          devkitPro build rules
source/engine.c   runtime: script interpreter, rendering, audio, input
source/webm.c     libvpx/libvorbis WebM demux and decoder
source/minimp3*   voice MP3 decoder
source/json_lite* minimal JSON parser for script.json
source/main.c     entry point and applet lifecycle
icon.jpg          homebrew menu icon
```

## Legal

*Class of '09* and its assets are the property of their developers and
publishers. **No game assets are included in this repository.** You must own
the game and generate/copy the assets from your own copy. Do not redistribute
the game assets.

The runtime contains no jailbreak, exploit, or DRM-circumvention code.
