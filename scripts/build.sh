#!/usr/bin/env bash
# Build slsteam-moon binaries.
#
# Two modes:
#   --host       Use the host toolchain (fast, dev iteration).
#                Binary's glibc requirement matches your system, so it
#                may not run on older distros than yours.
#   --portable   Build inside a container. i386: Ubuntu 22.04 (Podman or
#                Docker), binary needs glibc >= 2.34. x86_64: AlmaLinux
#                8.10 via scripts/build-portable-container.sh (Podman,
#                Docker or bubblewrap), which prints the measured floor;
#                see PORTING.md "Distribution floor". This is what release
#                builds use. Never falls back to a host build.
#
# Options:
#   --arch ARCH  Target architecture (default: i386, or x86_64 / aarch64;
#                --portable supports i386 and x86_64).
#
# Outputs:
#   bin/SLSsteam.so
#   bin/library-inject.so
#   bin/pattern-refresh
#
# Usage:
#   scripts/build.sh                 # defaults to --portable
#   scripts/build.sh --host
#   scripts/build.sh --host --arch x86_64
#   scripts/build.sh --portable
#   scripts/build.sh --portable --arch x86_64
#   scripts/build.sh --help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

MODE="portable"
while [ $# -gt 0 ]; do
	case "$1" in
		--host)     MODE="host"; shift ;;
		--portable) MODE="portable"; shift ;;
		--arch)
			export ARCH="${2:-}"
			shift 2
			;;
		--arch=*)
			export ARCH="${1#--arch=}"
			shift
			;;
		-h|--help)
			sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'
			exit 0
			;;
		*) echo "unknown option: \$1 (try --help)" >&2; exit 2 ;;
	esac
done

case "$MODE" in
	host)     "$SCRIPT_DIR/_build-host.sh" ;;
	portable) "$SCRIPT_DIR/_build-portable.sh" ;;
esac
