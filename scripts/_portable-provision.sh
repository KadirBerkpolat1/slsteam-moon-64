#!/usr/bin/env bash
# Install the build toolchain inside a portable-build container, as root.
# Invoked by scripts/build-portable-container.sh; not meant to run on a host.
#
#   _portable-provision.sh ARCH
#
# Toolchain choice is per base image, because the floor comes from the image's
# glibc and libstdc++, not from the compiler front end:
#   almalinux:8  gcc-toolset (newer g++ against the stock gcc 8 libstdc++.so;
#                symbols the old runtime lacks are linked statically from
#                libstdc++_nonshared.a, so GLIBCXX stays at the gcc 8 level)
#   debian/ubuntu  the distro g++

set -euo pipefail

ARCH="${1:?usage: _portable-provision.sh ARCH}"
GCC_TOOLSET="${GCC_TOOLSET:-13}"

. /etc/os-release

case "$ID" in
	almalinux|rocky|rhel|centos)
		pkgs=("gcc-toolset-$GCC_TOOLSET-gcc-c++" "gcc-toolset-$GCC_TOOLSET-binutils"
			make pkgconf-pkg-config findutils file diffutils
			libcurl-devel openssl-devel)
		if [ "$ARCH" != x86_64 ]; then
			pkgs+=(glibc-devel.i686 libgcc.i686 libstdc++-devel.i686
				"gcc-toolset-$GCC_TOOLSET-libstdc++-devel.i686")
		fi
		# Under bwrap only one uid/gid is mapped, so chown to any other group
		# fails; the toolset's chroot skeleton has exactly one such path
		# (var/spool/mail, group mail). Nothing in the build uses it.
		printf '%%_netsharedpath /opt/rh/gcc-toolset-%s/root/var/spool/mail\n' "$GCC_TOOLSET" \
			> /etc/rpm/macros.portable-netshared
		dnf -y --setopt=install_weak_deps=False --setopt=tsflags=nodocs install "${pkgs[@]}"
		dnf clean all
		if [ "$ARCH" != x86_64 ]; then
			# _build-host.sh looks for the Debian or Arch 32-bit pkg-config dir;
			# EL keeps i686 .pc files in /usr/lib/pkgconfig.
			mkdir -p /usr/lib32
			ln -sfn /usr/lib/pkgconfig /usr/lib32/pkgconfig
		fi
		printf '%s\n' "source /opt/rh/gcc-toolset-$GCC_TOOLSET/enable" > /etc/portable-env
		;;
	debian|ubuntu)
		apt_opts=(-o APT::Sandbox::User=root -o Dpkg::Options::=--force-overwrite
			-y --no-install-recommends)
		case "${VERSION_CODENAME:-}" in
			buster|bullseye)
				# Out of security support: deb.debian.org already 404s on the
				# security pool, archive.debian.org keeps the whole release.
				sed -i 's#https\?://\(deb\|security\)\.debian\.org#http://archive.debian.org#' \
					/etc/apt/sources.list /etc/apt/sources.list.d/* 2>/dev/null || true
				apt_opts+=(-o Acquire::Check-Valid-Until=false)
				;;
		esac
		pkgs=(ca-certificates make pkg-config file)
		if [ "$ARCH" = x86_64 ]; then
			pkgs+=(g++ libcurl4-openssl-dev libssl-dev)
		else
			dpkg --add-architecture i386
			pkgs+=(g++-multilib libcurl4-openssl-dev libssl-dev
				libcurl4-openssl-dev:i386 libssl-dev:i386)
		fi
		export DEBIAN_FRONTEND=noninteractive
		apt-get "${apt_opts[@]}" update
		apt-get "${apt_opts[@]}" install "${pkgs[@]}"
		rm -rf /var/lib/apt/lists/*
		if [ "$ARCH" = x86_64 ]; then
			: > /etc/portable-env
		else
			printf '%s\n' 'export PKG_CONFIG_PATH=/usr/lib/i386-linux-gnu/pkgconfig' > /etc/portable-env
		fi
		;;
	*)
		echo "_portable-provision: no recipe for $ID" >&2
		exit 1
		;;
esac
