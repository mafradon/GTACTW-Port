#!/usr/bin/env bash
# build-bullseye-sysroot.sh — assemble a minimal Debian 11 "bullseye" armhf
# glibc sysroot WITHOUT root/docker/debootstrap.
#
# Why: PortMaster's device matrix includes CFWs on glibc as old as 2.31
# (ArkOS-for-Clone, Debian 11 — GitHub issue #6).  Building on Ubuntu 24.04
# (glibc 2.39, 64-bit time_t) makes the binary require GLIBC_2.34 and
# GLIBC_2.38 symbols that those devices don't have, so it won't even load.
#
# bullseye ships glibc 2.31 with 32-bit time_t and pre-C23 headers, so a
# binary linked against it requires only <= GLIBC_2.31 symbols and runs on
# every current PortMaster CFW.
#
# We fetch just the glibc packages (libc + headers + crt objects) and extract
# them with dpkg-deb -x into a local dir.  The graphics/audio/z dev libs are
# reused from the existing armhf-sysroot: their symbols don't carry
# GLIBC_2.34+ version tags, so they don't affect the glibc requirement.
set -euo pipefail

MIRROR="http://deb.debian.org/debian"
SUITE="bullseye"
ARCH="armhf"
HERE="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$HERE/bullseye-sysroot"
ROOT="$OUT/root"
CACHE="$OUT/debs"

# glibc 2.31 and its build-time pieces. libc6-dev pulls headers + crt*.o +
# the libc.so linker script; libcrypt-dev is split out of glibc in 2.31.
#
# libmpg123 (1.26.4) is here for a SECOND, independent reason — the API version,
# not the glibc version.  Building against Ubuntu 24.04's mpg123 1.32 headers
# makes the binary import the mpg123 API-v2 symbols (mpg123_param2, mpg123_info2,
# mpg123_eq2, mpg123_getstate2, …), which simply do not exist in 1.26.4.  Debian
# 11 devices ship 1.26.4, so the game could never resolve them there — a hidden
# blocker sitting behind the GLIBC one in issue #6.
#
# 1.26.4's headers emit the LEGACY names instead, and mpg123 1.32 still exports
# those, so one binary then loads against both old and new system mpg123.
# Crucially, bullseye's 1.26.4 already exports all 22 large-file "_64" variants
# (mpg123_seek_64, mpg123_open_64, …), so the 64-bit off_t ABI is unchanged and
# the off_t bridges in src/mpg123_patch.c keep working as-is.
PKGS=(libc6 libc6-dev libc-dev-bin linux-libc-dev libcrypt1 libcrypt-dev
      libmpg123-0 libmpg123-dev)

mkdir -p "$ROOT" "$CACHE"

echo ">> Fetching Packages index ($SUITE/main/$ARCH)…"
IDX="$CACHE/Packages"
if [ ! -s "$IDX" ]; then
    curl -fsSL "$MIRROR/dists/$SUITE/main/binary-$ARCH/Packages.gz" \
        | gzip -d > "$IDX"
fi

# Given a package name, print its "Filename:" pool path (first stanza match).
pkg_filename() {
    awk -v pkg="$1" '
        $1=="Package:" { cur=$2 }
        $1=="Filename:" && cur==pkg { print $2; exit }
    ' "$IDX"
}

for p in "${PKGS[@]}"; do
    fn="$(pkg_filename "$p")"
    if [ -z "$fn" ]; then
        echo "!! package not found in index: $p" >&2
        exit 1
    fi
    deb="$CACHE/$(basename "$fn")"
    if [ ! -s "$deb" ]; then
        echo ">> Downloading $p"
        curl -fsSL "$MIRROR/$fn" -o "$deb"
    fi
    echo ">> Extracting $p"
    dpkg-deb -x "$deb" "$ROOT"
done

# Relativize absolute symlinks.  Debian ships dev symlinks like
#   libpthread.so -> /lib/arm-linux-gnueabihf/libpthread.so.0
# whose absolute target resolves against the real host root under --sysroot
# (i.e. nowhere), so ld silently falls back to the static libpthread.a and the
# link fails on glibc-internal symbols (__libc_do_syscall, _dl_pagesize, …).
# Rewrite every absolute symlink to point relatively inside the sysroot.
echo ">> Relativizing absolute symlinks…"
while IFS= read -r -d '' link; do
    tgt="$(readlink "$link")"
    case "$tgt" in
        /*) rel="$(realpath -m --relative-to="$(dirname "$link")" "$ROOT$tgt")"
            ln -sfn "$rel" "$link" ;;
    esac
done < <(find "$ROOT" -type l -print0)

# Graft KHR/khrplatform.h — required by <GLES2/gl2.h> but not shipped by the
# GLES dev package; it's architecture-neutral (pure compiler-macro typedefs),
# so the host copy is safe to reuse under --sysroot.
KHR_SRC=""
for c in /usr/include/KHR/khrplatform.h \
         "$HERE/armhf-sysroot/root/usr/include/KHR/khrplatform.h"; do
    [ -f "$c" ] && { KHR_SRC="$c"; break; }
done
if [ -n "$KHR_SRC" ]; then
    mkdir -p "$ROOT/usr/include/KHR"
    cp "$KHR_SRC" "$ROOT/usr/include/KHR/khrplatform.h"
    echo ">> grafted KHR/khrplatform.h from $KHR_SRC"
else
    echo "!! khrplatform.h not found — GLES2 headers will fail to compile" >&2
fi

echo ">> bullseye armhf sysroot ready at: $ROOT"
echo ">> glibc version:"
strings "$ROOT/lib/arm-linux-gnueabihf/libc.so.6" 2>/dev/null \
    | grep -m1 "GNU C Library" || true
