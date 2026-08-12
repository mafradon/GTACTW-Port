# Grand Theft Auto: Chinatown Wars (PortMaster)

A native **32-bit ARM (armhf)** port of the Android release of *GTA: Chinatown
Wars*. It runs the original `libCTW.so` game engine directly on Linux through a
custom Android-on-Linux loader (KMS/DRM video, GLES2, OpenAL audio) — no
emulation, no Box86.

You must own and supply the game's original **APK** and **OBB** data files.
None of Rockstar's copyrighted content is included in this port.

## Installation

1. Install this port through PortMaster (or copy this folder into your
   `ports/` directory).
2. Obtain from your own legally-owned copy:
   - `main.4.com.rockstargames.gtactw.obb` (~865 MB — game data)
   - `gtacw.apk` (~11–16 MB — text, fonts, legal screens)
3. Copy **both** files into:
   ```
   <ports>/GTA Chinatown Wars/gtactw/
   ```
4. Launch the port. The first run shows a setup screen (language + free-space
   check), then unpacks the engine and data with a progress bar (a few
   minutes), deletes the archives to reclaim space, and starts the game.

   You need about **890 MB free** beyond the archives themselves; the installer
   checks before it starts and refuses rather than dying half-way.

## First-run installer

`gtactw/installer.armhf` is a small standalone SDL2 program; the launcher runs
it only when the game data is not already unpacked. It decides that by looking
for three sentinel files — `ROM.WAD` (from the OBB), `GXT.obb.mp3` (from the
APK's `assets/`) and `libCTW.so` (the engine, from the APK's
`lib/armeabi-v7a/`) — so an interrupted install simply resumes on the next
launch, and a completed one is skipped entirely.

**Nothing copyrighted ships in this port.** The game engine, the game data and
the installer's background artwork are all lifted from *your own* APK and OBB
at install time.

It links **SDL2 only** — no GL, no OpenAL — and needs neither SDL2_image nor
SDL2_ttf, so it does not depend on the EGL/GLES path the game itself uses.

**No artwork is redistributed with this port.** The installer takes its
background out of *your own* APK at runtime — `res/drawable-xxxhdpi-v4/banner.png`
— decoding the PNG with zlib, and draws its text from a bitmap font compiled
into the binary. If `gtacw.apk` is not present it reports
`gtacw.apk file not found` as plain text and stops. If SDL cannot open a
window at all, it falls back to writing progress into `gtactw.log` rather than
extracting silently.

Exit codes: `0` installed (or nothing to do), `1` archives missing, `2`
extraction failed. Anything non-zero aborts the launch, and the launcher's
cleanup trap restores the console and the frontend.

## Controls

The game reads the controller natively through SDL's GameController API, so it
picks up PortMaster's per-device mapping automatically. **`gptokeyb` is disabled
by default** (`USE_GPTOKEYB=0` in the launcher) — a native-SDL port does not
need it, and letting it grab the pad can break in-game input. The Start+Select
quit combo is implemented by the game itself.

| Button         | Action            |
|----------------|-------------------|
| Left stick / D-pad | Move          |
| Face buttons   | In-game actions   |
| **Start + Select** | Quit the game |

## Language

The game is fully localised — **English, French, German, Italian, Spanish and
Japanese** — and picks a language from the device locale. Handhelds rarely set
`$LANG`, so by default everyone would get English.

To choose one, create a single-line file:

```
<ports>/GTA Chinatown Wars/gtactw/conf/language.txt
```

containing one of: `en` `fr` `de` `it` `es` `ja`

If the file is absent the port honours whatever `$LANG` the firmware exports;
if that is unset too, the game runs in English. An unrecognised value falls
back to English and is noted in `gtactw.log`.

No extra download is needed — all six languages are already inside the GXT that
the installer extracts from your APK.

## Device compatibility

- **Architecture:** 32-bit `armhf` only. The launcher sets `PORT_32BIT="Y"` so
  PortMaster provides its armhf runtime on aarch64-only firmware.
- **glibc:** built against **glibc 2.31** (Debian 11 bullseye), so it runs on
  CFWs as old as glibc 2.31 (ArkOS-for-Clone, Debian 11) as well as newer ones
  (AmberELEC, ROCKNIX). Earlier builds required GLIBC_2.34/2.38 and failed to
  load on older firmware.
- **GPU:** any GLES2 driver (tested on Mali-400 / Utgard and Mali-Bifrost G31).
  The device's own `libGLESv2`/`libEGL` vendor drivers are used — they are
  **not** bundled.
- **Ports location:** the launcher passes its own directory to the game via
  `GTACTW_DIR`, so it works from any mount (internal, SD1, SD2) and any CFW's
  ports path — not just `/roms/ports`.
- **Resolution:** renders at a fixed **640×480** (baked into the engine). On
  higher-res panels the firmware scales/letterboxes it.

### Bundled libraries (`gtactw/libs.armhf/`)

Self-contained armhf libraries shipped with the port:

- `libmpg123.so.0` (1.26.4, Debian bullseye), `libz.so.1`

**Every bundled library must have a glibc floor no higher than the executable's
(currently `GLIBC_2.29`).** A bundled library sits *first* on `LD_LIBRARY_PATH`,
so its own glibc requirement becomes a hard floor for the whole port — bundling
a lib built on a modern distro re-breaks exactly the old-glibc devices that the
bullseye build exists to support (issue #6). Check before adding anything:

```
readelf -V gtactw/libs.armhf/<lib> | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1
```

`libstdc++.so.6` and `libgcc_s.so.1` were previously bundled and have been
**removed**: nothing in the port links them (`gtactw.armhf`, `libmpg123` and
`libclock_fix.so` have no such `NEEDED` entry — `libCTW.so`'s `libstdc++.so` is
the *Android/Bionic* one, satisfied internally by the so_loader), and the copies
being shipped required `GLIBC_2.38`/`2.34`.

### ⚠️ SDL2 + OpenAL — deliberately NOT bundled

The game needs **`libSDL2-2.0.so.0`** and **`libopenal.so.1`** in armhf. These
are intentionally left to the device, and that is the *correct* design rather
than a gap: each device then supplies a copy built against its own glibc, which
is automatically compatible. Bundling either one would pin a glibc floor onto
every device (the armhf SDL2/OpenAL on current CFWs need `GLIBC_2.38`) and would
also risk shipping a desktop build that hard-links X11/Wayland/PulseAudio/sndio
and cannot load on a headless KMS handheld.

They resolve at runtime from, in order: `gtactw/libs.armhf/` → the device's
armhf system dirs (`/usr/lib/arm-linux-gnueabihf` on Debian-based CFWs,
`/usr/lib32` on JELOS/ROCKNIX/uOS) → PortMaster's 32-bit runtime (`PORT_32BIT=Y`).

On firmware that ships armhf SDL2 + OpenAL this works as-is. On aarch64-only
firmware it depends on PortMaster's armhf runtime being installed.

## Notes

- `libclock_fix.so` is `LD_PRELOAD`ed (into the game process only) to fix a
  broken 32-bit vDSO `clock_gettime` path seen on some Rockchip kernels. The
  binary also patches libc's `__clock_gettime64`/`__gettimeofday64` **only if
  the native function actually faults** — healthy glibc builds (AmberELEC,
  ROCKNIX) are left untouched, so the port no longer crashes with
  `SIGILL @ __clock_gettime64` on them.
- A log is written to `gtactw/gtactw.log` for troubleshooting.

## Credits

- **mafradon** — Linux/PortMaster port and Android-on-Linux loader.
- Original game by **Rockstar Leeds / Rockstar Games**.
- `gptokeyb`, SDL2, GL4ES, OpenAL Soft, mpg123, zlib — their respective authors
  (see `gtactw/licenses/`).
