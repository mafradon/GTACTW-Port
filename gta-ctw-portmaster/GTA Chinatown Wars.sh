#!/bin/bash
# GTA: Chinatown Wars — PortMaster launcher
# 32-bit (armhf) native port. Game data is supplied by the user (APK + OBB)
# and extracted on first run; see README.md.

# ──────────────────────────────────────────────────────────────────────────
# 1. Locate and source the PortMaster control library.
#    control.txt gives us: $ESUDO, $directory, $CFW_NAME, $DEVICE_ARCH,
#    $ANALOG_STICKS, $GPTOKEYB, $sdl_controllerconfig, get_controls,
#    pm_platform_helper, pm_finish, etc.
# ──────────────────────────────────────────────────────────────────────────
XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

[ -f "$controlfolder/control.txt" ] && source "$controlfolder/control.txt"

# This is a 32-bit (armhf) port. Tell PortMaster to provide its armhf runtime
# so the game's libraries resolve on aarch64-only firmware.
export PORT_32BIT="Y"

# Per-CFW overrides (frontend stop/restore, env quirks). Safe if absent.
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"

# Fallbacks so the script still runs from a plain SSH shell (no PortMaster).
ESUDO="${ESUDO:-sudo}"
DEVICE_ARCH="${DEVICE_ARCH:-armhf}"
CFW_NAME="${CFW_NAME:-unknown}"
type get_controls         >/dev/null 2>&1 || get_controls() { :; }
type pm_platform_helper   >/dev/null 2>&1 || pm_platform_helper() { :; }
type pm_finish            >/dev/null 2>&1 || pm_finish() { :; }

get_controls

# ──────────────────────────────────────────────────────────────────────────
# 2. Paths. Derive everything from the script's own location so this works
#    no matter where a given CFW mounts the ports folder.
# ──────────────────────────────────────────────────────────────────────────
PORTDIR="$(dirname "$(realpath "$0")")"
GAMEDIR="$PORTDIR/gtactw"
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR" || exit 1

CURR_TTY="/dev/tty1"
[ -e "$CURR_TTY" ] || CURR_TTY="/dev/tty0"

# Truncate the log; we tee to it below.
> "$GAMEDIR/gtactw.log"



# ──────────────────────────────────────────────────────────────────────────
# 3. Take the screen + input devices, stop the frontend, then arm the cleanup
#    trap. The trap goes up HERE, before anything that can exit early (the
#    installer can), so no failure path can leave the handheld with a stopped
#    frontend and a blank screen.
# ──────────────────────────────────────────────────────────────────────────
$ESUDO chmod 666 "$CURR_TTY"          2>/dev/null
$ESUDO chmod 666 /dev/uinput          2>/dev/null
$ESUDO chmod 666 /dev/dri/card0       2>/dev/null
$ESUDO chmod 666 /dev/dri/renderD128  2>/dev/null

# Cleanup: stop the helper, restore the console, restart the frontend.
#
# Run from a trap rather than straight-line after the game, so the device is
# never left with a blank screen: the frontend was stopped on the way in, and
# if we exit abnormally (SIGTERM/SIGINT from a kill, a crash in the game, or an
# ssh session dropping during a test run) the straight-line version never ran
# and the user had to physically reset the handheld to get a display back.
_cleanup() {
    [ -n "$_CLEANED" ] && return
    _CLEANED=1

    # Stop the game FIRST. If the launcher is signalled, the game is a separate
    # process and survives; restoring the console and the frontend underneath a
    # still-running game leaves two things fighting over the display, which is
    # the state that requires a physical reset. Give it a chance to exit
    # cleanly, then force it.
    if [ -n "$GAME_PID" ] && kill -0 "$GAME_PID" 2>/dev/null; then
        kill -TERM "$GAME_PID" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do
            kill -0 "$GAME_PID" 2>/dev/null || break
            sleep 0.5
        done
        kill -0 "$GAME_PID" 2>/dev/null && $ESUDO kill -9 "$GAME_PID" 2>/dev/null
    fi

    $ESUDO kill -9 $(pidof gptokeyb) 2>/dev/null

    echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
    echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1
    printf "\033c"   > "$CURR_TTY"
    printf "\e[?25h" > "$CURR_TTY"

    # Bring the frontend back. pm_finish is NOT defined by control.txt on
    # ArkOS/dArkOS (our fallback stub is a no-op), so relying on it alone
    # leaves the user staring at a black screen. Do it explicitly, exactly as
    # the previously-working launcher did.
    $ESUDO systemctl start emulationstation 2>/dev/null || \
      $ESUDO systemctl restart oga_events   2>/dev/null || true

    pm_finish
}
# EXIT covers the normal path and most failures; the signal traps additionally
# terminate the script (a bare handler would otherwise resume where it was
# interrupted). _CLEANED makes the second entry a no-op.
trap _cleanup EXIT
trap '_cleanup; exit 130' INT TERM HUP


# NOTE: we deliberately do NOT stop the frontend here.
#
# When the port is launched from EmulationStation, this script is a child
# inside the frontend's own systemd cgroup — so "systemctl stop
# emulationstation" kills US as well. That is exactly what happened: the trace
# showed "about to stop frontend" followed one second later by the cleanup
# trap, with the installer never reached. ES on ArkOS/dArkOS backgrounds
# itself when it launches a port, and PortMaster stops the frontend itself
# when the port is launched through PortMaster, so the stop is unnecessary in
# both paths and fatal in one of them.
#
# Releasing the vtconsole below is what actually frees the framebuffer for
# KMS; the frontend is restarted explicitly in _cleanup.
sleep 1


printf "\033c"   > "$CURR_TTY"   # clear
printf "\e[?25l" > "$CURR_TTY"   # hide cursor


# ──────────────────────────────────────────────────────────────────────────
# 3c. Runtime environment shared by BOTH the installer and the game.
#
# This has to come before the installer, not just before the game. SDL2's
# KMSDRM backend is GBM/EGL based — it has no plain-framebuffer path — so even
# the installer's 2D renderer loads EGL at window-creation time. Without these
# the installer fails with "Can't load EGL/GL library on window creation" and
# silently drops to text mode.
# ──────────────────────────────────────────────────────────────────────────
# Video — KMS/DRM, device's own GLES/EGL vendor drivers.
# Driver names match the proven config on the two working devices; do not
# "fix" these to versioned sonames without re-testing on hardware.
# Controller config string from PortMaster (per-device SDL mapping).
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

export SDL_VIDEODRIVER=kmsdrm
export SDL_VIDEO_GL_DRIVER=libGLESv2.so
export SDL_VIDEO_EGL_DRIVER=libEGL.so

# Library search order:
#   1. our bundled armhf libs (mpg123/z/stdc++/gcc_s, and SDL2/openal if the
#      porter dropped handheld builds into libs.armhf — see README),
#   2. the device's armhf system dirs. Debian-based CFWs (ArkOS) use the
#      multiarch path; buildroot-based CFWs (JELOS/ROCKNIX/uOS) use /usr/lib32.
#   3. whatever PortMaster's 32-bit runtime already put on the path.
#
# NOTE: this is deliberately "armhf", NOT "$DEVICE_ARCH".  DEVICE_ARCH is the
# arch of the DEVICE (aarch64 on most modern CFWs); this port's binary and its
# bundled libs are always 32-bit armhf.  Using $DEVICE_ARCH here pointed at a
# non-existent libs.aarch64 and silently dropped our bundled libs from the path.
export LD_LIBRARY_PATH="$GAMEDIR/libs.armhf:/usr/lib/arm-linux-gnueabihf:/usr/lib32:$LD_LIBRARY_PATH"

# ──────────────────────────────────────────────────────────────────────────
# 4. First-run installer.
#
# Sentinels: ROM.WAD comes from the OBB, GXT.obb.mp3 from the APK assets. When
# either is missing we hand over to installer.armhf — a standalone SDL2 splash
# that unpacks both archives and draws a progress bar over installer.bmp. It
# links SDL2 only (no GL), so it can still draw on a device where the game's
# EGL/GLES path is unhappy.
#
# The vtconsole is released BEFORE it runs: the installer is a KMS/DRM client
# just like the game and needs the framebuffer free. If SDL cannot open a
# window at all the installer degrades to printing progress into the log.
# ──────────────────────────────────────────────────────────────────────────
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1

# Let the installer put the console BACK if it cannot open an SDL window: its
# text fallback is useless while the framebuffer console is unbound.
$ESUDO chmod 666 /sys/class/vtconsole/vtcon0/bind 2>/dev/null
$ESUDO chmod 666 /sys/class/vtconsole/vtcon1/bind 2>/dev/null


if [ ! -f "$GAMEDIR/ROM.WAD" ] || [ ! -f "$GAMEDIR/GXT.obb.mp3" ]; then
    echo "launcher: game data missing — running installer" >> "$GAMEDIR/gtactw.log"
    # tee to the console as well as the log, so the text fallback is visible.
    ./installer.armhf "$GAMEDIR" 2>&1 | tee -a "$GAMEDIR/gtactw.log" > "$CURR_TTY"
    install_rc=${PIPESTATUS[0]}
    if [ "$install_rc" -ne 0 ]; then
        # The installer has already explained itself on screen and waited;
        # the EXIT trap below restores the console and the frontend.
        echo "launcher: installer exited $install_rc — aborting" >> "$GAMEDIR/gtactw.log"
        exit "$install_rc"
    fi
fi

# ──────────────────────────────────────────────────────────────────────────
# 4b. Language (read AFTER the installer, which may have just written it).
#
# The engine is fully localised (English, French, German, Italian, Spanish,
# Japanese — the GXT carries all six) and picks one from the device locale,
# which the port reports from $LANG. Handhelds almost never set $LANG, so
# without this everyone gets English regardless of the game's own setting.
#
# Order of preference:
#   1. gtactw/conf/language.txt  — one line: en | fr | de | it | es | ja
#   2. whatever $LANG the firmware already exports
#   3. English
# ──────────────────────────────────────────────────────────────────────────
LANG_FILE="$CONFDIR/language.txt"
if [ -f "$LANG_FILE" ]; then
    GAME_LANG="$(tr -d ' \t\r\n' < "$LANG_FILE" | tr '[:upper:]' '[:lower:]' | cut -c1-2)"
fi
case "$GAME_LANG" in
    en|fr|de|it|es|ja) ;;                  # supported
    "") GAME_LANG="" ;;                    # fall through to the device $LANG
    *)  echo "launcher: unsupported language '$GAME_LANG' in $LANG_FILE — using English" \
            >> "$GAMEDIR/gtactw.log"
        GAME_LANG="en" ;;
esac
if [ -n "$GAME_LANG" ]; then
    # Proper locale strings. The port only reads the first two characters, but
    # a well-formed value keeps anything else that reads $LANG happy.
    case "$GAME_LANG" in
        en) export LANG="en_US.UTF-8" ;;
        fr) export LANG="fr_FR.UTF-8" ;;
        de) export LANG="de_DE.UTF-8" ;;
        it) export LANG="it_IT.UTF-8" ;;
        es) export LANG="es_ES.UTF-8" ;;
        ja) export LANG="ja_JP.UTF-8" ;;
    esac
    echo "launcher: language = $GAME_LANG (LANG=$LANG)" >> "$GAMEDIR/gtactw.log"
fi

# ──────────────────────────────────────────────────────────────────────────
# 5. Game run. Log only from here; the game owns the framebuffer.
# ──────────────────────────────────────────────────────────────────────────
exec > >(tee -a "$GAMEDIR/gtactw.log") 2>&1

# Audio — ALSA is universal on these CFWs; OpenAL also targets ALSA.
export SDL_AUDIODRIVER="${SDL_AUDIODRIVER:-alsa}"
export AUDIODEV="${AUDIODEV:-default}"
export ALSOFT_DRIVERS="${ALSOFT_DRIVERS:-alsa}"

# Tell the 32-bit game binary where its data lives, instead of the compiled-in
# default (/roms/ports/gtactw).  Fixes launches from SD2 / non-standard mounts
# and other CFWs whose ports dir differs (GitHub issue #2).
export GTACTW_DIR="$GAMEDIR"

# Preload the versioned __clock_gettime64 override (works around a broken
# 32-bit vDSO clock_gettime path on some Rockchip kernels).  Applied ONLY to
# the game process below — NOT exported — so PortMaster's 64-bit helpers
# (pm_platform_helper, gptokeyb) don't reject a 32-bit preload with a noisy
# "wrong ELF class: ELFCLASS32" warning (GitHub issue #1).
GAME_PRELOAD="$GAMEDIR/libclock_fix.so"

# Controller → keyboard helper. DISABLED BY DEFAULT: the proven config on the
# two working devices ran with the game reading the gamepad natively via SDL,
# and gptokeyb may EVIOCGRAB the pad and break in-game input. Enable it (set
# USE_GPTOKEYB=1) only after confirming on hardware that controls still work;
# it adds the PortMaster Start+Select quit combo via gtactw.gptk.
USE_GPTOKEYB="${USE_GPTOKEYB:-0}"
if [ "$USE_GPTOKEYB" = "1" ] && [ -n "$GPTOKEYB" ]; then
    $GPTOKEYB "gtactw.armhf" -c "$GAMEDIR/gtactw.gptk" &
fi

pm_platform_helper "$GAMEDIR/gtactw.armhf"


# Run the game in the background and `wait` on it, so $GAME_PID is known to the
# trap. `wait` is interruptible by signals; a foreground child would not be.
LD_PRELOAD="$GAME_PRELOAD" ./gtactw.armhf &
GAME_PID=$!
wait "$GAME_PID"
