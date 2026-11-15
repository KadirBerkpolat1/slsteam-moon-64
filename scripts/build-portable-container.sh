#!/usr/bin/env bash
# Build the LD_AUDIT-loaded objects inside an old-glibc container and report the
# glibc / libstdc++ floor the result actually requires.
#
# SLSsteam.so and library-inject.so are LD_AUDIT-loaded into the host Steam
# process, so their version needs are checked against the *host* glibc and
# libstdc++ at load time. A build is only as portable as the oldest runtime it
# was linked against; this script pins that to an old image and measures the
# outcome instead of assuming it. The measured floors are in PORTING.md
# ("Distribution floor"); the CI recipe is in docs/portable-build.md.
#
# Options:
#   --arch ARCH         x86_64 (default) or i386
#   --image IMAGE       base image (default per arch, see DEFAULT_IMAGE below)
#   --runtime RT        podman, docker or bwrap (default: first usable, in that order)
#   --out DIR           output directory (default: dist/portable/ARCH)
#   --max-glibc VER     refuse if an LD_AUDIT object needs a newer GLIBC_ than VER
#   --max-glibcxx VER   refuse if an LD_AUDIT object needs a newer GLIBCXX_ than VER
#                       (both default to the measured floor of the default image)
#   --with-helper       also build bin/pattern-refresh, in HELPER_IMAGE
#   --netsock64 FILE    also build FILE (moon64's patches/netsock64/
#                       steamclient_audit64.c) into netsock64.so, x86_64 only,
#                       in IMAGE with the moon64 installer's gcc command
#
# bin/pattern-refresh is a host-side helper, not an LD_AUDIT object, and links
# the image's libcurl (>= 7.66 for curl_multi_poll) and OpenSSL 3, which the
# AlmaLinux 8 image does not have. --with-helper builds it in a second pass in
# HELPER_IMAGE and gates it with scripts/check-pattern-refresh-abi.sh.
#
# netsock64.so is LD_AUDIT-loaded into Steam like SLSsteam.so, so it is built
# in the same image, load-checked under that image's ld.so, and counted in the
# measured floors.
#
# Builds a clean snapshot of HEAD (uncommitted changes are NOT included) with
# scripts/build.sh --host inside the container. Never falls back to a host
# build: with no usable runtime it exits non-zero.
#
# Outputs in DIR: SLSsteam.so, library-inject.so, [netsock64.so],
#                 [pattern-refresh], SHA256SUMS, manifest.json, abi-floor.txt
#
# Usage:
#   scripts/build-portable-container.sh
#   scripts/build-portable-container.sh --arch i386
#   scripts/build-portable-container.sh --image docker.io/library/ubuntu:22.04 \
#       --max-glibc 2.35 --max-glibcxx 3.4.30

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# Floors measured from these images are recorded in PORTING.md. Changing an
# image means re-measuring and updating the ADVERTISED_* values.
declare -A DEFAULT_IMAGE=(
	[x86_64]=docker.io/library/almalinux:8.10
	[i386]=docker.io/library/ubuntu:22.04
)
declare -A ADVERTISED_GLIBC=([x86_64]=2.17 [i386]=2.34)
declare -A ADVERTISED_GLIBCXX=([x86_64]=3.4.22 [i386]=3.4.30)
HELPER_IMAGE=docker.io/library/ubuntu:22.04

ARCH=x86_64
IMAGE=""
RUNTIME=""
OUT=""
MAX_GLIBC=""
MAX_GLIBCXX=""
WITH_HELPER=0
NETSOCK_SRC=""
AUDIT_OBJECTS=(SLSsteam.so library-inject.so)

die() { echo "build-portable-container: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--arch)        ARCH="${2:?}"; shift 2 ;;
		--image)       IMAGE="${2:?}"; shift 2 ;;
		--runtime)     RUNTIME="${2:?}"; shift 2 ;;
		--out)         OUT="${2:?}"; shift 2 ;;
		--max-glibc)   MAX_GLIBC="${2:?}"; shift 2 ;;
		--max-glibcxx) MAX_GLIBCXX="${2:?}"; shift 2 ;;
		--with-helper) WITH_HELPER=1; shift ;;
		--netsock64)   NETSOCK_SRC="${2:?}"; shift 2 ;;
		-h|--help)     sed -n '2,46p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*)             die "unknown option: $1 (try --help)" ;;
	esac
done

case "$ARCH" in
	x86_64) ;;
	i386|i486|i586|i686|x86|x86_32) ARCH=i386 ;;
	*) die "unsupported --arch $ARCH (x86_64 or i386)" ;;
esac
if [ -z "$IMAGE" ]; then
	IMAGE="${DEFAULT_IMAGE[$ARCH]}"
	MAX_GLIBC="${MAX_GLIBC:-${ADVERTISED_GLIBC[$ARCH]}}"
	MAX_GLIBCXX="${MAX_GLIBCXX:-${ADVERTISED_GLIBCXX[$ARCH]}}"
fi
OUT="${OUT:-$REPO_ROOT/dist/portable/$ARCH}"
if [ -n "$NETSOCK_SRC" ]; then
	[ "$ARCH" = x86_64 ] || die "--netsock64 is x86_64 only"
	[ -f "$NETSOCK_SRC" ] || die "--netsock64 $NETSOCK_SRC: no such file"
fi

for tool in git tar readelf objdump sha256sum; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done

usable() {
	case "$1" in
		podman) command -v podman >/dev/null 2>&1 && podman info >/dev/null 2>&1 ;;
		docker) command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1 ;;
		bwrap)  command -v bwrap >/dev/null 2>&1 && command -v python3 >/dev/null 2>&1 &&
		        bwrap --unshare-user --uid 0 --ro-bind / / true >/dev/null 2>&1 ;;
		*)      return 1 ;;
	esac
}

if [ -n "$RUNTIME" ]; then
	usable "$RUNTIME" || die "--runtime $RUNTIME is not usable on this host"
else
	for rt in podman docker bwrap; do
		if usable "$rt"; then RUNTIME="$rt"; break; fi
	done
fi
if [ -z "$RUNTIME" ]; then
	cat >&2 <<-EOF
	build-portable-container: no usable container runtime.

	Tried, in order: podman, docker (daemon reachable), bubblewrap (unprivileged
	user namespaces + python3). Install one of them; a host build would link
	against this machine's glibc and NOT give the advertised floor, so this
	script will not do one. For a host build use scripts/build.sh --host.
	EOF
	exit 1
fi

COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD)"
if [ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no)" ]; then
	echo "==> note: uncommitted changes are not part of this build (building $COMMIT)" >&2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/slsteam-portable.XXXXXX")"
cleanup() { chmod -R u+w "$WORK" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

JOBS="$(nproc 2>/dev/null || echo 2)"
LAST_DIGEST=""

# snapshot DIR: a clean tree of COMMIT plus the in-container build step.
snapshot() {
	mkdir -p "$1"
	git -C "$REPO_ROOT" archive --format=tar "$COMMIT" | tar -x -C "$1"
	cp "$SCRIPT_DIR/_portable-build.sh" "$1/.portable-build.sh"
}

# container_run IMAGE PROVISION_ARCH DIR -- CMD...
# Runs CMD in DIR, mounted at /build, inside IMAGE provisioned for
# PROVISION_ARCH. Sets LAST_DIGEST to the image digest.
container_run() {
	local image="$1" prov_arch="$2" src="$3"
	shift 4

	case "$RUNTIME" in
		podman|docker)
			local ctx="$WORK/ctx-$prov_arch" tag run_opts
			mkdir -p "$ctx"
			cp "$SCRIPT_DIR/_portable-provision.sh" "$ctx/"
			cat > "$ctx/Containerfile" <<-EOF
			FROM $image
			COPY _portable-provision.sh /usr/local/sbin/_portable-provision.sh
			RUN bash /usr/local/sbin/_portable-provision.sh $prov_arch
			WORKDIR /build
			EOF
			tag="localhost/slsteam-moon-portable:$prov_arch-$(printf '%s' "$image" | tr -c 'A-Za-z0-9_.-' '-')"
			"$RUNTIME" pull "$image" >&2
			LAST_DIGEST="$("$RUNTIME" image inspect --format '{{range .RepoDigests}}{{println .}}{{end}}' "$image" |
				sed -n '1s/.*@//p')"
			"$RUNTIME" build -f "$ctx/Containerfile" -t "$tag" "$ctx" >&2
			run_opts=(--rm -v "$src:/build:Z" -w /build -e "MAKEFLAGS=-j$JOBS")
			# Rootful docker would leave root-owned files in $WORK; rootless
			# podman already maps container root to the invoking user.
			[ "$RUNTIME" = docker ] && run_opts+=(--user "$(id -u):$(id -g)" -e HOME=/tmp)
			"$RUNTIME" run "${run_opts[@]}" "$tag" "$@"
			;;
		bwrap)
			local cache="${XDG_CACHE_HOME:-$HOME/.cache}/slsteam-moon/portable" key root
			key="$( { printf '%s\n%s\n' "$image" "$prov_arch"; cat "$SCRIPT_DIR/_portable-provision.sh"; } |
				sha256sum | cut -c1-16)"
			root="$cache/$key"
			if [ ! -f "$root/ready" ]; then
				chmod -R u+w "$root" 2>/dev/null || true
				rm -rf "$root"
				mkdir -p "$root"
				echo "==> fetching $image into $root" >&2
				python3 "$SCRIPT_DIR/_oci-rootfs.py" "$image" "$root/rootfs" > "$root/digest"
				rm -f "$root/rootfs/etc/resolv.conf"
				: > "$root/rootfs/etc/resolv.conf"
				bw "$root/rootfs" --ro-bind "$SCRIPT_DIR/_portable-provision.sh" /provision.sh \
					bash /provision.sh "$prov_arch" >&2
				touch "$root/ready"
			fi
			LAST_DIGEST="$(cat "$root/digest")"
			bw "$root/rootfs" --bind "$src" /build --chdir /build "$@"
			;;
	esac
}

# --cap-add ALL grants capabilities inside the user namespace only; rpm and dpkg
# need CAP_DAC_OVERRIDE to write into the 0555 directories they create.
bw() {
	local rootfs="$1"; shift
	bwrap --unshare-user --uid 0 --gid 0 --cap-add ALL \
		--unshare-ipc --unshare-pid --unshare-uts --die-with-parent \
		--bind "$rootfs" / --dev /dev --proc /proc --tmpfs /tmp \
		--ro-bind /etc/resolv.conf /etc/resolv.conf \
		--clearenv --setenv HOME /root --setenv LANG C.UTF-8 \
		--setenv PATH /usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin \
		--setenv MAKEFLAGS "-j$JOBS" \
		"$@"
}

stage="$WORK/out"
mkdir -p "$stage"

echo "==> portable build: arch=$ARCH image=$IMAGE runtime=$RUNTIME commit=$COMMIT"
snapshot "$WORK/src"
container_run "$IMAGE" "$ARCH" "$WORK/src" -- bash .portable-build.sh "$ARCH" "${AUDIT_OBJECTS[@]/#/bin/}"
IMAGE_DIGEST="$LAST_DIGEST"
for f in "${AUDIT_OBJECTS[@]}"; do
	[ -s "$WORK/src/bin/$f" ] || die "container build did not produce bin/$f"
	cp "$WORK/src/bin/$f" "$stage/$f"
done
LAV="$(tr -d '[:space:]' < "$WORK/src/bin/.lav-current")"
ARTIFACTS=("${AUDIT_OBJECTS[@]}")

if [ -n "$NETSOCK_SRC" ]; then
	echo "==> netsock64.so from $NETSOCK_SRC in $IMAGE"
	mkdir -p "$WORK/netsock64"
	cp "$NETSOCK_SRC" "$WORK/netsock64/steamclient_audit64.c"
	cat > "$WORK/netsock64/build.sh" <<-'EOF'
	set -euo pipefail
	source /etc/portable-env
	gcc --version | sed -n 1p
	gcc -O2 -Wall -Wextra -shared -fPIC -o netsock64.so steamclient_audit64.c -ldl
	out="$(LD_AUDIT="$PWD/netsock64.so" /bin/true 2>&1)" || { echo "$out" >&2; exit 1; }
	if [ -n "$out" ]; then
		echo "this image's ld.so rejected netsock64.so: $out" >&2
		exit 1
	fi
	echo "==> netsock64.so loads as an LD_AUDIT module under this image's ld.so"
	EOF
	container_run "$IMAGE" "$ARCH" "$WORK/netsock64" -- bash build.sh
	[ "$LAST_DIGEST" = "$IMAGE_DIGEST" ] ||
		die "$IMAGE changed digest between the two builds ($IMAGE_DIGEST, $LAST_DIGEST)"
	[ -s "$WORK/netsock64/netsock64.so" ] || die "container build did not produce netsock64.so"
	cp "$WORK/netsock64/netsock64.so" "$stage/netsock64.so"
	AUDIT_OBJECTS+=(netsock64.so)
	ARTIFACTS+=(netsock64.so)
fi

HELPER_DIGEST=""
if [ "$WITH_HELPER" = 1 ]; then
	echo "==> helper build: bin/pattern-refresh in $HELPER_IMAGE"
	snapshot "$WORK/src-helper"
	container_run "$HELPER_IMAGE" x86_64 "$WORK/src-helper" -- bash .portable-build.sh x86_64 bin/pattern-refresh
	HELPER_DIGEST="$LAST_DIGEST"
	[ -s "$WORK/src-helper/bin/pattern-refresh" ] || die "helper build did not produce bin/pattern-refresh"
	cp "$WORK/src-helper/bin/pattern-refresh" "$stage/pattern-refresh"
	"$SCRIPT_DIR/check-pattern-refresh-abi.sh" "$stage/pattern-refresh"
	ARTIFACTS+=(pattern-refresh)
fi

max_of() { printf '%s\n' "$@" | sed '/^$/d' | sort -V | tail -1; }
floor() { "$SCRIPT_DIR/abi-floor.sh" --floor "$1" "$2"; }
audit_floor() {
	local ns="$1" f vals=()
	for f in "${AUDIT_OBJECTS[@]}"; do vals+=("$(floor "$ns" "$stage/$f")"); done
	max_of "${vals[@]}"
}

(cd "$stage" && "$SCRIPT_DIR/abi-floor.sh" "${ARTIFACTS[@]}") > "$stage/abi-floor.txt"
cat "$stage/abi-floor.txt"

glibc="$(audit_floor GLIBC)"
glibcxx="$(audit_floor GLIBCXX)"
cxxabi="$(audit_floor CXXABI)"

if [ -n "$MAX_GLIBC" ] && [ "$(max_of "$glibc" "$MAX_GLIBC")" != "$MAX_GLIBC" ]; then
	die "measured GLIBC_$glibc exceeds the advertised GLIBC_$MAX_GLIBC for $IMAGE; refusing to write $OUT"
fi
if [ -n "$MAX_GLIBCXX" ] && [ "$(max_of "$glibcxx" "$MAX_GLIBCXX")" != "$MAX_GLIBCXX" ]; then
	die "measured GLIBCXX_$glibcxx exceeds the advertised GLIBCXX_$MAX_GLIBCXX for $IMAGE; refusing to write $OUT"
fi

(cd "$stage" && sha256sum "${ARTIFACTS[@]}" > SHA256SUMS)

# Top-level floors cover the LD_AUDIT objects, the ones checked against the
# host when Steam starts; pattern-refresh carries its own entry under files.
{
	printf '{\n'
	printf '  "arch": "%s",\n' "$ARCH"
	printf '  "glibc_floor": "%s",\n' "$glibc"
	printf '  "glibcxx_floor": "%s",\n' "$glibcxx"
	printf '  "cxxabi_floor": "%s",\n' "$cxxabi"
	printf '  "audit_interface": %s,\n' "$LAV"
	printf '  "source_commit": "%s",\n' "$COMMIT"
	printf '  "built_at": "%s",\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf '  "image": "%s",\n' "$IMAGE"
	printf '  "image_digest": "%s",\n' "$IMAGE_DIGEST"
	if [ "$WITH_HELPER" = 1 ]; then
		printf '  "helper_image": "%s",\n' "$HELPER_IMAGE"
		printf '  "helper_image_digest": "%s",\n' "$HELPER_DIGEST"
	fi
	if [ -n "$NETSOCK_SRC" ]; then
		printf '  "netsock64_source_sha256": "%s",\n' "$(sha256sum "$NETSOCK_SRC" | cut -d' ' -f1)"
	fi
	printf '  "runtime": "%s",\n' "$RUNTIME"
	printf '  "files": {\n'
	sep=""
	for f in "${ARTIFACTS[@]}"; do
		printf '%s    "%s": {"glibc_floor": "%s", "glibcxx_floor": "%s", "cxxabi_floor": "%s", "sha256": "%s"}' \
			"$sep" "$f" "$(floor GLIBC "$stage/$f")" "$(floor GLIBCXX "$stage/$f")" \
			"$(floor CXXABI "$stage/$f")" "$(sha256sum "$stage/$f" | cut -d' ' -f1)"
		sep=$',\n'
	done
	printf '\n  }\n}\n'
} > "$stage/manifest.json"

rm -rf "$OUT"
mkdir -p "$(dirname "$OUT")"
cp -r "$stage" "$OUT"

echo "==> measured floor ($ARCH, $IMAGE): GLIBC_$glibc GLIBCXX_$glibcxx CXXABI_$cxxabi"
echo "==> $IMAGE <link.h> has LAV_CURRENT $LAV; la_version negotiation is not checked here (build-release-payload.sh's floor-image load check does)"
echo "==> wrote $OUT"
