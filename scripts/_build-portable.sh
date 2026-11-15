#!/usr/bin/env bash
# Portable build implementation. Invoked by scripts/build.sh --portable.
#
#   i386    Builds inside an Ubuntu 22.04 container (scripts/Dockerfile). The
#           native helper is then checked separately so it cannot require any
#           glibc symbol newer than GLIBC_2.34.
#   x86_64  Delegates to scripts/build-portable-container.sh, which builds the
#           LD_AUDIT objects in AlmaLinux 8.10 and pattern-refresh in Ubuntu
#           22.04, measures the floor, and refuses on a regression. The results
#           are then copied into bin/ like every other build.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR/.."

ARCH_TO_BUILD="${ARCH:-i386}"

if [ "$ARCH_TO_BUILD" = x86_64 ]; then
	out="dist/portable/x86_64"
	"$SCRIPT_DIR/build-portable-container.sh" --arch x86_64 --with-helper --out "$out"
	# What `make clean` inside the old container did for obj/ and bin/.
	rm -rf obj bin
	mkdir -p bin
	cp "$out/SLSsteam.so" "$out/library-inject.so" "$out/pattern-refresh" bin/
	echo "==> built: bin/SLSsteam.so, bin/library-inject.so, bin/pattern-refresh (manifest: $out/manifest.json)"
	exit 0
fi

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

case "$ARCH_TO_BUILD" in
	i386|i486|i586|i686|x86|x86_32)
		dockerfile="scripts/Dockerfile"
		image="slsteam-moon-builder"
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

echo "==> built: bin/SLSsteam.so, bin/library-inject.so, bin/pattern-refresh"
