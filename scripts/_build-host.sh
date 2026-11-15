#!/usr/bin/env bash
# Host build implementation. Invoked by scripts/build.sh --host.
# Compiles in place using the host's toolchain. Fast for development;
# binary glibc minimum equals your system's glibc.
#
# BUILD_TARGETS (env) narrows the make goals, e.g. to the two LD_AUDIT-loaded
# objects when the image cannot build the host-only pattern-refresh helper.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

HOST_ARCH="$(uname -m)"
if [ -z "${ARCH:-}" ]; then
	if [ "$HOST_ARCH" = "aarch64" ]; then
		ARCH="aarch64"
	else
		ARCH="i386"
	fi
fi

while [ $# -gt 0 ]; do
	case "$1" in
		--arch)
			ARCH="${2:-}"
			shift 2
			;;
		--arch=*)
			ARCH="${1#--arch=}"
			shift
			;;
		*)
			echo "unknown option: $1" >&2
			exit 2
			;;
	esac
done

echo "==> host build (arch=$ARCH, glibc $(ldd --version | head -1 | awk '{print $NF}'))"

missing=()
command -v g++  >/dev/null 2>&1 || missing+=("g++")
command -v make >/dev/null 2>&1 || missing+=("make")

case "$ARCH" in
	i386|i486|i586|i686|x86|x86_32)
		# 32-bit headers are required (-m32). Sniff for either pkg-config layout.
		if [ -d /usr/lib/i386-linux-gnu/pkgconfig ]; then
			export PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig
		elif [ -d /usr/lib32/pkgconfig ]; then
			export PKG_CONFIG_PATH=/usr/lib32/pkgconfig
		else
			missing+=("32-bit dev libs (libssl-dev:i386, libcurl4-openssl-dev:i386, or distro equivalent)")
		fi

		if [ ${#missing[@]} -gt 0 ]; then
			echo "missing dependencies:" >&2
			for d in "${missing[@]}"; do echo "  - $d" >&2; done
			cat >&2 <<-EOF

			Install on Ubuntu/Debian:
			  sudo dpkg --add-architecture i386
			  sudo apt update
			  sudo apt install g++-multilib make libssl-dev:i386 libcurl4-openssl-dev:i386

			Install on Arch:
			  sudo pacman -S multilib/lib32-gcc-libs multilib/lib32-openssl multilib/lib32-curl

			Or build inside a container instead:  scripts/build.sh --portable
			EOF
			exit 1
		fi
		;;
	x86_64)
		# 64-bit native headers/libs. Sniff Debian/Ubuntu, RHEL/Fedora, and Arch layouts.
		pc_dir=""
		for d in /usr/lib/x86_64-linux-gnu/pkgconfig /usr/lib64/pkgconfig /usr/lib/pkgconfig; do
			if [ -d "$d" ] && [ -f "$d/libcurl.pc" ] && [ -f "$d/libssl.pc" ]; then
				pc_dir="$d"
				break
			fi
		done
		if [ -n "$pc_dir" ]; then
			export PKG_CONFIG_PATH="$pc_dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
		elif ! (command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl libssl); then
			missing+=("64-bit dev libs (libssl-dev, libcurl4-openssl-dev, or distro equivalent)")
		fi

		if [ ${#missing[@]} -gt 0 ]; then
			echo "missing dependencies:" >&2
			for d in "${missing[@]}"; do echo "  - $d" >&2; done
			cat >&2 <<-EOF

			Install on Ubuntu/Debian:
			  sudo apt update
			  sudo apt install g++ make libssl-dev libcurl4-openssl-dev pkg-config

			Install on Arch:
			  sudo pacman -S gcc make openssl curl pkgconf
			EOF
			exit 1
		fi
		;;
	aarch64)
		pc_dir=""
		for d in /usr/lib/aarch64-linux-gnu/pkgconfig /usr/lib64/pkgconfig /usr/lib/pkgconfig; do
			if [ -d "$d" ] && [ -f "$d/libcurl.pc" ] && [ -f "$d/libssl.pc" ]; then
				pc_dir="$d"
				break
			fi
		done
		if [ -n "$pc_dir" ]; then
			export PKG_CONFIG_PATH="$pc_dir${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
		elif ! (command -v pkg-config >/dev/null 2>&1 && pkg-config --exists libcurl libssl); then
			missing+=("aarch64 dev libs (libssl-dev, libcurl4-openssl-dev, or distro equivalent)")
		fi

		if [ ${#missing[@]} -gt 0 ]; then
			echo "missing dependencies:" >&2
			for d in "${missing[@]}"; do echo "  - $d" >&2; done
			exit 1
		fi
		;;
	*)
		echo "unsupported ARCH: $ARCH (expected i386/i686/x86, x86_64, or aarch64)" >&2
		exit 2
		;;
esac

make clean
# shellcheck disable=SC2086 # BUILD_TARGETS is a space-separated goal list
make ARCH="$ARCH" ${BUILD_TARGETS:-}
echo "==> built ($ARCH): ${BUILD_TARGETS:-bin/SLSsteam.so bin/library-inject.so bin/pattern-refresh}"
