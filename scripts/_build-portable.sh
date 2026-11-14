#!/usr/bin/env bash
# Portable build implementation. Invoked by scripts/build.sh --portable.
# Builds inside an Ubuntu 22.04 container. The native helper is then checked
# separately so it cannot require any glibc symbol newer than GLIBC_2.34.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

# Pick a container runtime: prefer rootless podman, fall back to docker.
runtime=""
for c in podman docker; do
	if command -v "$c" >/dev/null 2>&1; then runtime="$c"; break; fi
done
if [ -z "$runtime" ]; then
	cat >&2 <<-EOF
	No container runtime found.

	Install one of these and try again:
	  Podman (rootless, recommended):
	    Ubuntu/Debian: sudo apt install podman
	    Fedora:        sudo dnf install podman
	    Arch:          sudo pacman -S podman
	  Docker:
	    Ubuntu/Debian: sudo apt install docker.io && sudo systemctl enable --now docker
	    Fedora:        sudo dnf install docker
	    Arch:          sudo pacman -S docker
	    (then: sudo usermod -aG docker \$USER && newgrp docker)

	Or build directly on your host:  scripts/build.sh --host
	EOF
	exit 1
fi

# The two arches need different toolchains, not just different flags:
#   i386   Ubuntu 22.04 (glibc 2.35) is enough and keeps the 2.34 floor.
#   x86_64 the source is C++20 and <cstdlib> redirects strtol to __isoc23_*
#          (glibc 2.38), and io_base_library_init needs GLIBCXX_3.4.32.
#          Ubuntu 22.04 cannot link it, so this arch uses 24.04.
ARCH_TO_BUILD="${ARCH:-i386}"
case "$ARCH_TO_BUILD" in
	i386|i486|i586|i686|x86|x86_32)
		dockerfile="scripts/Dockerfile"
		image="slsteam-moon-builder"
		;;
	x86_64)
		dockerfile="scripts/Dockerfile.x86_64"
		image="slsteam-moon-builder-x86_64"
		;;
	*)
		echo "no portable builder for ARCH=$ARCH_TO_BUILD" >&2
		echo "supported: i386, x86_64" >&2
		exit 1
		;;
esac

echo "==> portable build (using $runtime, image $image, arch $ARCH_TO_BUILD)"

# Build (or reuse) the builder image. Tagging by name only, the layer cache
# inside the runtime keeps rebuilds cheap.
"$runtime" build -f "$dockerfile" -t "$image" . >&2

# Run the build inside the container with the repo bind-mounted, so the
# resulting bin/ lands on the host without needing a docker cp.
"$runtime" run --rm \
	-v "$PWD:/build:Z" \
	-w /build \
	"$image" \
	bash -c "make clean && make ARCH=$ARCH_TO_BUILD"

scripts/check-pattern-refresh-abi.sh bin/pattern-refresh

if [ "$ARCH_TO_BUILD" = x86_64 ]; then
	ceiling="$(readelf -V bin/SLSsteam.so | grep -oE 'GLIBC_2\.[0-9]+' | sort -uV | tail -1)"
	echo "==> SLSsteam.so glibc floor: $ceiling (host $(getconf GNU_LIBC_VERSION | awk '{print $2}'))"
fi

echo "==> built: bin/SLSsteam.so, bin/library-inject.so, bin/pattern-refresh"
