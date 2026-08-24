#!/bin/bash
# Regression harness for the launcher's display-back-end decision.
#
# The launcher decides ONE thing early -- who owns the panel -- and that
# decision gates the SDL video driver, the vtconsole unbind/rebind, the
# /dev/dri chmods, the tty writes and the frontend restart. Getting it wrong on
# the kmsdrm path would break every device that currently works, and none of it
# can be exercised on a build box.
#
# So: run the real launcher against a throwaway port directory with every
# privileged command replaced by a stub that just logs its arguments, and the
# game binary replaced by one that dumps the environment it was handed. What
# comes out is an exact trace of what the launcher would DO on a device.
#
#   ./scripts/test-launcher-display.sh                  # test both paths
#   ./scripts/test-launcher-display.sh <old-launcher>   # + diff vs a baseline
#
# The load-bearing assertion is the last one: with a baseline given, the SET of
# privileged actions on the kmsdrm path must be unchanged. Ordering may differ.
set -u

REPO="$(dirname "$(dirname "$(realpath "$0")")")"
LAUNCHER="$REPO/gta-ctw-portmaster/GTA Chinatown Wars.sh"
BASELINE="${1:-}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fails=0

# Build a rig and run one launcher through it. $1=mode $2=launcher path.
# Modes: kmsdrm (no compositor), wayland (fake socket), conf (override file).
run_rig() {
    # NOTE: separate statements on purpose. In `local a="$1" b="$a"` bash
    # expands every word before applying any assignment, so $mode would still
    # be unset (and unbound under `set -u`) in the same command.
    local mode="$1" src="$2"
    local rig="$WORK/$mode.$RANDOM"
    mkdir -p "$rig/gtactw/conf" "$rig/bin" "$rig/run"

    # Point the tty writes at a file so the rig never touches a real console.
    sed -e "s#CURR_TTY=\"/dev/tty1\"#CURR_TTY=\"$rig/TTY\"#" \
        -e 's#\[ -e "$CURR_TTY" \] || CURR_TTY="/dev/tty0"#touch "$CURR_TTY"#' \
        "$src" > "$rig/GTA Chinatown Wars.sh"

    # Stub every privileged command: log the call, do nothing.
    local c
    # pkill/pgrep matter because control.txt's own pm_finish calls them; an
    # unstubbed pkill would run for real against the device.
    for c in sudo chmod tee systemctl pidof kill pkill pgrep; do
        { echo '#!/bin/bash'
          echo "echo \"CALL: $c \$*\" >> \"$rig/trace\""
          [ "$c" = tee ] && echo 'cat > /dev/null'
          echo 'exit 0'; } > "$rig/bin/$c"
        chmod +x "$rig/bin/$c"
    done

    # Stub game + installer: record the environment we were handed.
    { echo '#!/bin/bash'
      echo "{ echo \"GAME ENV:\""
      echo '  for v in SDL_VIDEODRIVER SDL_VIDEO_GL_DRIVER SDL_VIDEO_EGL_DRIVER \'
      echo '           WAYLAND_DISPLAY XDG_RUNTIME_DIR GTACTW_DIR; do'
      echo '      echo "  $v=${!v}"'
      echo "  done; } >> \"$rig/trace\""
      echo 'exit 0'; } > "$rig/gtactw/gtactw.armhf"
    chmod +x "$rig/gtactw/gtactw.armhf"
    cp "$rig/gtactw/gtactw.armhf" "$rig/gtactw/installer.armhf"

    # Sentinels present -> the installer is skipped, we test the game path.
    touch "$rig/gtactw/ROM.WAD" "$rig/gtactw/GXT.obb.mp3" "$rig/gtactw/libclock_fix.so"

    case "$mode" in
        wayland) touch "$rig/run/wayland-1" "$rig/run/wayland-1.lock" ;;
        conf)    echo wayland > "$rig/gtactw/conf/videodriver.txt" ;;
    esac

    : > "$rig/trace"
    env -i HOME="$rig" PATH="$rig/bin:/usr/bin:/bin" XDG_RUNTIME_DIR="$rig/run" \
        bash "$rig/GTA Chinatown Wars.sh" >> "$rig/trace" 2>&1
    cat "$rig/gtactw/gtactw.log" >> "$rig/trace" 2>/dev/null

    # Each run gets its own throwaway directory, so paths differ between the
    # baseline run and the current one. Normalise them away or the regression
    # diff reports noise instead of behaviour.
    sed -i "s#$rig#RIG#g" "$rig/trace"
    echo "$rig/trace"
}

check() { # $1=description $2=trace $3=grep pattern $4=expect(yes|no)
    if grep -q "$3" "$2"; then found=yes; else found=no; fi
    if [ "$found" = "$4" ]; then
        printf '  PASS  %s\n' "$1"
    else
        printf '  FAIL  %s (expected %s, got %s)\n' "$1" "$4" "$found"; fails=$((fails+1))
    fi
}

echo "== bash -n =="
if bash -n "$LAUNCHER"; then echo "  PASS  syntax"; else echo "  FAIL  syntax"; fails=$((fails+1)); fi

echo "== kmsdrm path (no compositor) =="
K="$(run_rig kmsdrm "$LAUNCHER")"
check "selects kmsdrm"                  "$K" 'SDL_VIDEODRIVER=kmsdrm'          yes
check "claims display ownership"        "$K" 'OWNS_DISPLAY=1'                  yes
check "unbinds the vtconsole"           "$K" 'tee /sys/class/vtconsole'        yes
check "chmods the DRM card node"        "$K" 'chmod 666 /dev/dri/card0'        yes
check "restarts the frontend"           "$K" 'systemctl start emulationstation' yes
check "pins the GLES driver"            "$K" 'SDL_VIDEO_GL_DRIVER=libGLESv2.so' yes

echo "== wayland path (compositor owns the display) =="
W="$(run_rig wayland "$LAUNCHER")"
check "selects wayland"                 "$W" 'SDL_VIDEODRIVER=wayland'         yes
check "disclaims display ownership"     "$W" 'OWNS_DISPLAY=0'                  yes
check "exports WAYLAND_DISPLAY"         "$W" 'WAYLAND_DISPLAY=wayland-1'       yes
check "leaves the vtconsole alone"      "$W" 'tee /sys/class/vtconsole'        no
check "leaves the DRM card node alone"  "$W" 'chmod 666 /dev/dri/card0'        no
check "does not restart the frontend"   "$W" 'systemctl start emulationstation' no
# Bare libGLESv2.so/libEGL.so are dev-package symlinks; a runtime-only image may
# ship only .so.2/.so.1. Forcing them where it was never tested reintroduces
# issue #9. The Wayland run in issue #8 set neither.
check "does NOT pin the GLES driver"    "$W" 'SDL_VIDEO_GL_DRIVER=libGLESv2.so' no
check "still grants uinput (gptokeyb)"  "$W" 'chmod 666 /dev/uinput'           yes

echo "== conf/videodriver.txt override =="
C="$(run_rig conf "$LAUNCHER")"
check "override wins over autodetect"   "$C" 'SDL_VIDEODRIVER=wayland'         yes
check "override is logged"              "$C" 'from conf/videodriver.txt'       yes

if [ -n "$BASELINE" ]; then
    echo "== regression vs baseline: $BASELINE =="
    B="$(run_rig kmsdrm "$BASELINE")"
    # Compare the SET of privileged actions, not their order.
    if diff <(grep '^CALL:' "$B" | sort) <(grep '^CALL:' "$K" | sort) > "$WORK/d"; then
        echo "  PASS  kmsdrm performs an identical set of actions"
    else
        echo "  FAIL  kmsdrm action set changed:"; sed 's/^/        /' "$WORK/d"; fails=$((fails+1))
    fi
fi

echo
if [ "$fails" -eq 0 ]; then echo "ALL CHECKS PASSED"; else echo "$fails CHECK(S) FAILED"; fi
exit "$fails"
