#!/usr/bin/env bash
# Build step run inside a portable-build container, from the source snapshot
# root. Invoked by scripts/build-portable-container.sh; not meant for a host.
#
#   _portable-build.sh ARCH TARGET...
#
# Builds TARGETs through scripts/build.sh --host, records the LD_AUDIT interface
# version the image's <link.h> makes la_version() request, and for x86_64 checks
# every built .so as an audit module under this image's own ld.so with
# .audit-load-check.sh (scripts/_audit-load-check.sh), so a build the image's
# loader rejects or silently ignores never leaves the container.

set -euo pipefail

ARCH="${1:?usage: _portable-build.sh ARCH TARGET...}"
shift
[ $# -gt 0 ] || { echo "_portable-build: no targets" >&2; exit 2; }

# shellcheck disable=SC1091 # written by _portable-provision.sh in the image
source /etc/portable-env
BUILD_TARGETS="$*" bash scripts/build.sh --host --arch "$ARCH"
printf '#include <link.h>\nLAV_CURRENT\n' | g++ -E -P -x c++ - | tail -1 > bin/.lav-current

# An i386 object cannot audit the image's 64-bit /bin/true.
[ "$ARCH" = x86_64 ] || exit 0
for target in "$@"; do
	case "$target" in *.so) ;; *) continue ;; esac
	lc="$(sh .audit-load-check.sh "$PWD/$target")"
	head="$(printf '%s\n' "$lc" | head -1)"
	msg="$(printf '%s\n' "$lc" | tail -n +2)"
	case "$head" in
		*" rc=0 la_objopen_lookups="[1-9]*) [ -z "$msg" ] || { echo "_portable-build: this image's ld.so complained about $target: $msg" >&2; exit 1; } ;;
		*) echo "_portable-build: this image's ld.so did not accept $target as an audit module: $head $msg" >&2; exit 1 ;;
	esac
	echo "==> $target accepted by this image's ld.so as an LD_AUDIT module: rc 0, no message, la_objopen lookups ${head##*la_objopen_lookups=}"
done
