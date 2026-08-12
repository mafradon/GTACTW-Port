# Licenses / Attribution

Verbatim upstream license texts for the third-party components this port bundles
or depends on are in this folder.

| Component        | Role in this port                  | Bundled? | License | Text |
|------------------|------------------------------------|----------|---------|------|
| libCTW.so        | Original game engine (© Rockstar Games) | **No — extracted from the player's own APK at install time** | Proprietary | — |
| mpg123           | MP3 decode (radio / music)         | Yes (`libs.armhf/libmpg123.so.0`, Debian bullseye 1.26.4) | LGPL-2.1 | `LICENSE.mpg123` |
| zlib             | Decompression                      | Yes (`libs.armhf/libz.so.1`) | zlib | `LICENSE.zlib` |
| SDL2             | Windowing / input / audio backend  | No — from device or PortMaster armhf runtime | zlib | `LICENSE.SDL2` |
| OpenAL Soft      | 3D audio (`dlopen`ed at runtime)   | No — from device or PortMaster armhf runtime | **LGPL-2** (Library GPL v2, not 2.1) | `LICENSE.OpenAL-Soft` |
| gptokeyb         | Controller → keyboard / quit hotkey| No — provided by PortMaster; this port ships only `gtactw.gptk` and leaves it disabled by default | GPL-2.0 | `LICENSE.gptokeyb` |
| Terminus Font    | Bitmap glyphs compiled into `installer.armhf` (printable ASCII of Terminus Bold 16) so the installer needs no SDL2_ttf | Yes — embedded in the binary | SIL OFL 1.1 | `LICENSE.Terminus-Font` |

`libstdc++` / `libgcc_s` were previously bundled and have been **removed** —
nothing in the port links them, and the shipped copies required GLIBC_2.38/2.34,
which would have broken old-glibc devices (see the port README).

No Rockstar Games content is redistributed here: the engine, the game data
and the installer's background artwork all come from the player's own files. The player supplies
their own legally-owned APK and OBB, which are extracted on first launch.
