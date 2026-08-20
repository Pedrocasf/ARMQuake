#!/bin/sh
#
# Build the armv7 (32-bit, hard-float) binary without a cross-toolchain.
#
# Alpine ships no arm-linux cross-gcc, so instead we drive an armv7 rootfs's
# own native gcc under qemu-arm user-mode emulation.  Three things make that
# work without root:
#
#   * gcc is relocatable -- run as $ROOTFS/usr/bin/gcc it computes its install
#     prefix from its own path, so it locates its cc1 and internal headers.
#   * --sysroot points the system header and library search at the rootfs
#     rather than the x86_64 host's /usr.
#   * -B<wrappers> shadows cc1/as/ld/collect2 with host shell scripts that
#     re-enter qemu.  Without a binfmt_misc handler for ARM the kernel cannot
#     exec those ARM binaries when gcc spawns them, and registering one needs
#     root.  A #! script is host-executable, so this sidesteps it.
#
# Usage:  ./build-armv7.sh [path-to-armv7-rootfs]
#
# The result lands in releasearmv7/quake and can be run with:
#   qemu-arm -L $ROOTFS ./releasearmv7/quake
#
set -e

ROOTFS=${1:-/home/pedrocasf/armv7chroot}
[ $# -gt 0 ] && shift		# anything further is passed on to make
WRAP=${WRAP:-$(pwd)/.armv7-wrappers}

if [ ! -x "$ROOTFS/usr/bin/gcc" ]; then
	echo "error: no armv7 gcc at $ROOTFS/usr/bin/gcc" >&2
	exit 1
fi
if [ ! -f "$ROOTFS/usr/include/SDL2/SDL.h" ]; then
	echo "error: no SDL2 headers in $ROOTFS (need sdl2-dev)" >&2
	exit 1
fi
command -v qemu-arm >/dev/null || { echo "error: qemu-arm not installed" >&2; exit 1; }

# gcc's version-specific libexec directory, e.g.
# usr/libexec/gcc/armv7-alpine-linux-musleabihf/15.2.0
LIBEXEC=$(find "$ROOTFS/usr/libexec/gcc" -name cc1 -print -quit 2>/dev/null | xargs -r dirname)
[ -n "$LIBEXEC" ] || { echo "error: cannot find cc1 under $ROOTFS/usr/libexec/gcc" >&2; exit 1; }

rm -rf "$WRAP"; mkdir -p "$WRAP"
mkwrap() {
	printf '#!/bin/sh\nexec qemu-arm -L %s %s "$@"\n' "$ROOTFS" "$2" > "$WRAP/$1"
	chmod +x "$WRAP/$1"
}
mkwrap cc1         "$LIBEXEC/cc1"
mkwrap cc1plus     "$LIBEXEC/cc1plus"
mkwrap collect2    "$LIBEXEC/collect2"
mkwrap lto1        "$LIBEXEC/lto1"
mkwrap lto-wrapper "$LIBEXEC/lto-wrapper"
mkwrap as          "$ROOTFS/usr/bin/as"
mkwrap ld          "$ROOTFS/usr/bin/ld"
mkwrap ld.bfd      "$ROOTFS/usr/bin/ld.bfd"

printf '#!/bin/sh\nexec qemu-arm -L %s %s/usr/bin/gcc --sysroot=%s -B%s "$@"\n' \
	"$ROOTFS" "$ROOTFS" "$ROOTFS" "$WRAP" > "$WRAP/armv7-gcc"
chmod +x "$WRAP/armv7-gcc"

# SDL_CFLAGS must use host-absolute paths: -I is not sysroot-relative.
# -lSDL2 is, so SDL_LIBS needs no prefix.
exec make -f Makefile.ARM \
	ARCH=armv7 \
	CC="$WRAP/armv7-gcc" \
	SDL_CFLAGS="-I$ROOTFS/usr/include/SDL2 -D_REENTRANT" \
	SDL_LIBS="-lSDL2" \
	"$@"
