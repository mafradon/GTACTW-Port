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
#
# ESUDO: do NOT blindly fall back to "sudo". Some images (the Sway/ROCKNIX
# builds in GitHub issue #8) run the frontend as root and ship no sudo at all,
# so a blind fallback turns every privileged line below into "sudo: command not
# found". Root needs no escalation; a non-root shell only gets sudo if it is
# actually installed. An explicit ESUDO="" from control.txt is left alone.
if [ "$(id -u)" -eq 0 ]; then
    ESUDO=""
elif [ -z "${ESUDO+x}" ]; then
    if command -v sudo >/dev/null 2>&1; then ESUDO="sudo"; else ESUDO=""; fi
elif [ -n "$ESUDO" ] && ! command -v "${ESUDO%% *}" >/dev/null 2>&1; then
    # First WORD only. PortMaster's control.txt does not set a bare command
    # name here -- on dArkOS it is
    #   ESUDO="sudo --preserve-env=SDL_GAMECONTROLLERCONFIG_FILE,DEVICE,..."
    # and testing the whole string with command -v looks up one executable with
    # that impossible name, finds nothing, and wrongly blanks a perfectly good
    # ESUDO. Everything privileged then runs unprivileged and fails, including
    # the frontend restart in _cleanup -- i.e. a dead screen.
    ESUDO=""
fi
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
# 2b. Display back-end. This decides TWO things at once, which is why it has
#     to happen this early: which SDL video driver we ask for, AND whether we
#     are entitled to touch the console and the DRM device at all.
#
# On every CFW tested so far the device boots to a bare KMS console and this
# port is the only DRM client, so it takes the framebuffer for itself: unbind
# the vtconsole, chmod the card node, draw straight to the tty. That is the
# kmsdrm path and it stays the default.
#
# Some images (ROCKNIX/dArkOS builds running Sway) already have a Wayland
# compositor holding the DRM master. There kmsdrm cannot open the device at
# all -- "SDL_Init: kmsdrm not available" -- while SDL's wayland backend works
# fine (GitHub issue #8). On that path the whole console/DRM dance is not just
# unnecessary, it is actively wrong: the framebuffer is not ours to seize.
#
# Precedence:
#   1. gtactw/conf/videodriver.txt   -- one line: wayland | kmsdrm | x11 | ...
#   2. a pre-set $SDL_VIDEODRIVER inherited from the environment
#   3. autodetection of a running Wayland compositor
#   4. kmsdrm
# ──────────────────────────────────────────────────────────────────────────
VIDEO_FILE="$CONFDIR/videodriver.txt"
if [ -f "$VIDEO_FILE" ]; then
    SDL_VIDEODRIVER="$(tr -d ' \t\r\n' < "$VIDEO_FILE" | tr '[:upper:]' '[:lower:]')"
    [ -n "$SDL_VIDEODRIVER" ] && \
        echo "launcher: video driver = $SDL_VIDEODRIVER (from conf/videodriver.txt)" \
            >> "$GAMEDIR/gtactw.log"
fi

if [ -z "$SDL_VIDEODRIVER" ]; then
    # $WAYLAND_DISPLAY is the direct signal, but a frontend may launch the port
    # with a stripped environment, so also look for the compositor's socket.
    #
    # $DISPLAY is deliberately NOT consulted: the Sway images in issue #8 export
    # DISPLAY=:0.0 as well, so it discriminates nothing and an X11-first chain
    # would mis-fire on exactly the devices this is meant to fix.
    _wl_dir="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
    _wl_sock="$(ls "$_wl_dir"/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)"

    # Widen the search if that came up empty. These images run the frontend as
    # root, so the default guess is /run/user/0 -- which often does not exist,
    # while the compositor's socket sits under some other uid's runtime dir.
    [ -z "$_wl_sock" ] && \
        _wl_sock="$(ls /run/user/*/wayland-* 2>/dev/null | grep -v '\.lock$' | head -n1)"

    if [ -n "$WAYLAND_DISPLAY" ] || [ -n "$_wl_sock" ]; then
        SDL_VIDEODRIVER="wayland"
        # Hand SDL what it needs to reach the compositor even when we got here
        # by finding the socket ourselves rather than by inheriting the vars.
        # The runtime dir must come from where the socket ACTUALLY is, not from
        # the guess above, or SDL looks in the wrong place.
        [ -n "$_wl_sock" ] && export XDG_RUNTIME_DIR="$(dirname "$_wl_sock")"
        : "${XDG_RUNTIME_DIR:=$_wl_dir}"; export XDG_RUNTIME_DIR
        [ -z "$WAYLAND_DISPLAY" ] && [ -n "$_wl_sock" ] && \
            export WAYLAND_DISPLAY="$(basename "$_wl_sock")"
        echo "launcher: Wayland compositor detected (WAYLAND_DISPLAY=$WAYLAND_DISPLAY," \
             "XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR) -- using the wayland video driver" \
             >> "$GAMEDIR/gtactw.log"
    else
        SDL_VIDEODRIVER="kmsdrm"
    fi
fi

# OWNS_DISPLAY=1 means "nothing else is driving the panel, so the console and
# the DRM node are ours to take". Every tty/vtconsole/card0 block below is
# gated on it; under a compositor we leave all of that alone.
case "$SDL_VIDEODRIVER" in
    kmsdrm) OWNS_DISPLAY=1 ;;
    *)      OWNS_DISPLAY=0 ;;
esac

# Where the installer's on-screen text goes. With no console of our own there
# is nowhere to print it but the log.
if [ "$OWNS_DISPLAY" = "1" ]; then CONSOLE_OUT="$CURR_TTY"; else CONSOLE_OUT="/dev/null"; fi

echo "launcher: SDL_VIDEODRIVER=$SDL_VIDEODRIVER OWNS_DISPLAY=$OWNS_DISPLAY" \
    >> "$GAMEDIR/gtactw.log"



# ──────────────────────────────────────────────────────────────────────────
# 3. Take the screen + input devices, stop the frontend, then arm the cleanup
#    trap. The trap goes up HERE, before anything that can exit early (the
#    installer can), so no failure path can leave the handheld with a stopped
#    frontend and a blank screen.
# ──────────────────────────────────────────────────────────────────────────
# /dev/uinput is for gptokeyb and is unrelated to who owns the screen, so it
# is opened either way. The tty and the DRM nodes are only ours on the kmsdrm
# path -- under a compositor the card node already has a master and loosening
# its permissions achieves nothing.
$ESUDO chmod 666 /dev/uinput          2>/dev/null
if [ "$OWNS_DISPLAY" = "1" ]; then
    $ESUDO chmod 666 "$CURR_TTY"          2>/dev/null
    $ESUDO chmod 666 /dev/dri/card0       2>/dev/null
    $ESUDO chmod 666 /dev/dri/renderD128  2>/dev/null
fi

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

    # Undo the console seizure -- but only if we performed it. Rebinding the
    # vtconsole underneath a live Wayland compositor would disturb a session we
    # never touched on the way in.
    if [ "$OWNS_DISPLAY" = "1" ]; then
        echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
        echo 1 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1
        printf "\033c"   > "$CURR_TTY"
        printf "\e[?25h" > "$CURR_TTY"

        # Bring the frontend back. pm_finish is NOT defined by control.txt on
        # ArkOS/dArkOS (our fallback stub is a no-op), so relying on it alone
        # leaves the user staring at a black screen. Do it explicitly, exactly
        # as the previously-working launcher did.
        #
        # Gated with the rest: this is a repair for a stop that this launcher
        # never performs (see the note further down), and it is only known to be
        # needed on the console-owning CFWs. On a compositor-driven image the
        # frontend is a Wayland client that never went away, and pm_finish below
        # covers the case where PortMaster itself stopped it.
        $ESUDO systemctl start emulationstation 2>/dev/null || \
          $ESUDO systemctl restart oga_events   2>/dev/null || true
    fi

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


if [ "$OWNS_DISPLAY" = "1" ]; then
    printf "\033c"   > "$CURR_TTY"   # clear
    printf "\e[?25l" > "$CURR_TTY"   # hide cursor
fi


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

# SDL_VIDEODRIVER was decided in section 2b (kmsdrm by default, wayland when a
# compositor owns the display); export the result rather than forcing kmsdrm.
export SDL_VIDEODRIVER

# The unversioned driver names are the PROVEN values on the kmsdrm devices, so
# they stay -- but only there. They are dev-package symlinks: a runtime-only
# image may ship just libGLESv2.so.2 / libEGL.so.1, in which case forcing the
# bare names makes SDL's loader fail with "Can't load EGL/GL library on window
# creation" (the failure from issue #9). The Wayland run reported in issue #8
# reached SDL_GL_MakeCurrent with SDL's OWN default EGL/GL loading and none of
# these set, so on that path we leave SDL to do what was demonstrated to work.
if [ "$OWNS_DISPLAY" = "1" ]; then
    export SDL_VIDEO_GL_DRIVER=libGLESv2.so
    export SDL_VIDEO_EGL_DRIVER=libEGL.so
fi

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
# Only on the kmsdrm path: under a compositor the framebuffer is already the
# compositor's, and unbinding the vtconsole would not hand it to us anyway.
if [ "$OWNS_DISPLAY" = "1" ]; then
    echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon0/bind > /dev/null 2>&1
    echo 0 | $ESUDO tee /sys/class/vtconsole/vtcon1/bind > /dev/null 2>&1

    # Let the installer put the console BACK if it cannot open an SDL window:
    # its text fallback is useless while the framebuffer console is unbound.
    $ESUDO chmod 666 /sys/class/vtconsole/vtcon0/bind 2>/dev/null
    $ESUDO chmod 666 /sys/class/vtconsole/vtcon1/bind 2>/dev/null
fi


if [ ! -f "$GAMEDIR/ROM.WAD" ] || [ ! -f "$GAMEDIR/GXT.obb.mp3" ]; then
    echo "launcher: game data missing — running installer" >> "$GAMEDIR/gtactw.log"
    # tee to the console as well as the log, so the text fallback is visible.
    ./installer.armhf "$GAMEDIR" 2>&1 | tee -a "$GAMEDIR/gtactw.log" > "$CONSOLE_OUT"
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
