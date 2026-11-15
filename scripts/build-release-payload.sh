#!/usr/bin/env bash
# Build the x86_64 release payload that `moon64 install.sh --prebuilt` consumes,
# in one shot, from HEAD.
#
# Output in DIR (default dist/release/x86_64), nothing else:
#   SLSsteam.so          built by scripts/build-portable-container.sh (AlmaLinux 8.10)
#   SLSsteam.so.sha256   "<sha256>  SLSsteam.so"
#   pattern-status.txt   the pattern table measured on THIS SLSsteam.so in this
#                        run, with binary_sha256= naming it (see "Binding" below)
#
# Steps, each of which refuses (exit 1, DIR untouched) on failure:
#   1. Container build. Symbol floor gated at GLIBC_2.17, GLIBCXX_3.4.22 and
#      CXXABI_1.3.11, the floor PORTING.md's working-floor numbers rest on.
#   2. Load check: the library must be accepted as an LD_AUDIT module by the
#      stock FLOOR_IMAGE's own ld.so and libstdc++ (default ubuntu:18.04,
#      glibc 2.27, the measured working floor).
#   3. Pattern measurement: the library is LD_AUDIT-loaded into an inert host
#      named "steam" that dlopens steamclient.so and steamui.so from
#      --steam-runtime. It runs in bubblewrap with the whole filesystem
#      read-only, no network and an empty HOME, so the Steam install is only
#      read. The census comes from the library's own runtime attestation
#      (every event carries the sha256 of the loaded file) and the pattern
#      catalog it writes once every required pattern resolved.
#   4. The measured census must equal res/pattern-status.txt at HEAD (arch,
#      required, resolved, pending, runipcframe_tail) and pass the installer's
#      gate (pending=0, runipcframe_tail=ready).
#
# Binding: pattern-status.txt is an output of the run that built and measured
# the library, and its single binary_sha256= line is the sha256 of the
# SLSsteam.so beside it. A consumer must refuse the counts unless binary_sha256
# equals the sha256 of the library it downloaded.
#
# Options:
#   --out DIR            payload directory (default dist/release/x86_64)
#   --steam-runtime DIR  directory with steamclient.so and steamui.so to measure
#                        against (default ~/.local/share/Steam/steamrt64)
#   --runtime RT         container runtime for the build (podman, docker, bwrap)
#   --floor-image IMAGE  image for the load check (default ubuntu:18.04)
#   --no-load-check      skip step 2; the payload is still built, and the run
#                        reports the working floor as not measured
#
# Needs: git, bwrap, python3, cc, readelf, objdump, sha256sum.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

MAX_GLIBC=2.17
MAX_GLIBCXX=3.4.22
MAX_CXXABI=1.3.11
FLOOR_IMAGE=docker.io/library/ubuntu:18.04

OUT="$REPO_ROOT/dist/release/x86_64"
STEAM_RT="$HOME/.local/share/Steam/steamrt64"
RUNTIME=""
LOAD_CHECK=1

die() { echo "build-release-payload: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
	case "$1" in
		--out)           OUT="${2:?}"; shift 2 ;;
		--steam-runtime) STEAM_RT="${2:?}"; shift 2 ;;
		--runtime)       RUNTIME="${2:?}"; shift 2 ;;
		--floor-image)   FLOOR_IMAGE="${2:?}"; shift 2 ;;
		--no-load-check) LOAD_CHECK=0; shift ;;
		-h|--help)       sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*)               die "unknown option: $1 (try --help)" ;;
	esac
done

for tool in git bwrap python3 cc readelf objdump sha256sum; do
	command -v "$tool" >/dev/null 2>&1 || die "$tool is required"
done
for lib in steamclient.so steamui.so; do
	[ -f "$STEAM_RT/$lib" ] || die "$STEAM_RT/$lib not found (set --steam-runtime)"
done
OUT="$(realpath -m "$OUT")"
STEAM_RT="$(realpath "$STEAM_RT")"

COMMIT="$(git -C "$REPO_ROOT" rev-parse HEAD)"
COMMITTED_STATUS="$(git -C "$REPO_ROOT" show HEAD:res/pattern-status.txt)" ||
	die "res/pattern-status.txt is not committed at HEAD"
if [ -n "$(git -C "$REPO_ROOT" status --porcelain --untracked-files=no)" ]; then
	echo "==> note: uncommitted changes are not part of this payload (building $COMMIT)" >&2
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/slsteam-release.XXXXXX")"
cleanup() { chmod -R u+w "$WORK" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

version_le() { [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -1)" = "$2" ]; }

# --- 1. build ---------------------------------------------------------------
echo "==> [1/4] container build of $COMMIT"
build_args=(--arch x86_64 --out "$WORK/build" --max-glibc "$MAX_GLIBC" --max-glibcxx "$MAX_GLIBCXX")
[ -z "$RUNTIME" ] || build_args+=(--runtime "$RUNTIME")
"$SCRIPT_DIR/build-portable-container.sh" "${build_args[@]}"
SO="$WORK/build/SLSsteam.so"
[ -s "$SO" ] || die "the container build produced no SLSsteam.so"
SHA="$(sha256sum "$SO" | cut -d' ' -f1)"

glibc_floor="$("$SCRIPT_DIR/abi-floor.sh" --floor GLIBC "$SO")"
glibcxx_floor="$("$SCRIPT_DIR/abi-floor.sh" --floor GLIBCXX "$SO")"
cxxabi_floor="$("$SCRIPT_DIR/abi-floor.sh" --floor CXXABI "$SO")"
version_le "${glibc_floor:-0}" "$MAX_GLIBC" || die "SLSsteam.so needs GLIBC_$glibc_floor, above GLIBC_$MAX_GLIBC"
version_le "${glibcxx_floor:-0}" "$MAX_GLIBCXX" || die "SLSsteam.so needs GLIBCXX_$glibcxx_floor, above GLIBCXX_$MAX_GLIBCXX"
version_le "${cxxabi_floor:-0}" "$MAX_CXXABI" || die "SLSsteam.so needs CXXABI_$cxxabi_floor, above CXXABI_$MAX_CXXABI"
echo "==> symbol floor: GLIBC_$glibc_floor GLIBCXX_$glibcxx_floor CXXABI_$cxxabi_floor"

# --- 2. load check on the floor image ---------------------------------------
load_checked="not measured (--no-load-check)"
if [ "$LOAD_CHECK" = 1 ]; then
	echo "==> [2/4] LD_AUDIT load check under $FLOOR_IMAGE"
	cache="${XDG_CACHE_HOME:-$HOME/.cache}/slsteam-moon/loadcheck/$(printf '%s' "$FLOOR_IMAGE" | tr -c 'A-Za-z0-9_.-' '-')"
	if [ ! -f "$cache/ready" ]; then
		chmod -R u+w "$cache" 2>/dev/null || true
		rm -rf "$cache"
		mkdir -p "$cache"
		python3 "$SCRIPT_DIR/_oci-rootfs.py" "$FLOOR_IMAGE" "$cache/rootfs" > "$cache/digest"
		touch "$cache/ready"
	fi
	mkdir -p "$WORK/lc"
	cp "$SO" "$WORK/lc/SLSsteam.so"
	cat > "$WORK/lc/check.sh" <<-'EOF'
	so=/tmp/lc/SLSsteam.so
	glibc=""
	for l in /lib/x86_64-linux-gnu/libc.so.6 /lib64/libc.so.6 /usr/lib64/libc.so.6; do
		[ -x "$l" ] && { glibc=$("$l" 2>&1 | head -1 | sed 's/.*version \([0-9.]*\).*/\1/; s/\.$//'); break; }
	done
	out=$(LD_AUDIT=$so /bin/true 2>&1); rc=$?
	n=$(LD_DEBUG=symbols LD_AUDIT=$so /bin/true 2>&1 |
		awk -v f="lookup in file=$so" 'index($0, "symbol=la_objopen;") && index($0, f) {n++} END {print n+0}')
	printf 'glibc=%s rc=%s la_objopen_lookups=%s\n' "$glibc" "$rc" "$n"
	printf '%s\n' "$out"
	EOF
	lc_out="$(bwrap --unshare-user --uid 0 --gid 0 --unshare-net --unshare-pid --die-with-parent \
		--ro-bind "$cache/rootfs" / --dev /dev --proc /proc --tmpfs /tmp \
		--ro-bind "$WORK/lc" /tmp/lc --clearenv --setenv PATH /usr/bin:/bin \
		/bin/sh /tmp/lc/check.sh)"
	printf '%s\n' "$lc_out" | sed 's/^/    /'
	lc_head="$(printf '%s\n' "$lc_out" | head -1)"
	lc_msg="$(printf '%s\n' "$lc_out" | tail -n +2)"
	lc_glibc="$(printf '%s' "$lc_head" | sed -n 's/^glibc=\([0-9.]*\) .*/\1/p')"
	case "$lc_head" in
		*" rc=0 la_objopen_lookups="[1-9]*) [ -z "$lc_msg" ] || die "$FLOOR_IMAGE ld.so complained: $lc_msg" ;;
		*) die "$FLOOR_IMAGE ld.so did not accept SLSsteam.so as an audit module: $lc_head $lc_msg" ;;
	esac
	load_checked="$FLOOR_IMAGE@$(cat "$cache/digest") (glibc $lc_glibc)"
else
	echo "==> [2/4] load check skipped"
fi

# --- 3. pattern measurement --------------------------------------------------
echo "==> [3/4] pattern measurement against $STEAM_RT"
H="$WORK/harness"
mkdir -p "$H/home/.config" "$H/home/.cache" "$H/home/.local/share" "$H/run"
chmod 700 "$H/run"
cp "$SO" "$H/SLSsteam.so"
cat > "$WORK/steam.c" <<-'EOF'
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char** argv)
{
	for (int i = 1; i < argc; ++i)
	{
		if (!dlopen(argv[i], RTLD_NOW | RTLD_GLOBAL))
		{
			fprintf(stderr, "dlopen %s: %s\n", argv[i], dlerror());
			return 1;
		}
	}
	sleep(2);
	return 0;
}
EOF
cc -O2 -o "$H/steam" "$WORK/steam.c" -ldl

set +e
timeout 180 bwrap --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp --bind "$H" /tmp/h \
	--ro-bind "$STEAM_RT" "$STEAM_RT" \
	--unshare-net --unshare-pid --unshare-ipc --die-with-parent \
	--clearenv --setenv HOME /tmp/h/home --setenv XDG_RUNTIME_DIR /tmp/h/run \
	--setenv PATH /usr/bin:/bin --setenv LD_LIBRARY_PATH "$STEAM_RT" \
	--setenv LD_AUDIT /tmp/h/SLSsteam.so \
	--setenv SLSSTEAM_ATTESTATION_FILE /tmp/h/attestation.jsonl \
	--setenv SLSSTEAM_ATTESTATION_SESSION release-payload \
	/tmp/h/steam "$STEAM_RT/steamclient.so" "$STEAM_RT/steamui.so" > "$H/stdout" 2>&1
hrc=$?
set -e
[ "$hrc" = 0 ] || { cat "$H/stdout" >&2; die "the measurement host exited $hrc"; }

MEASURED="$(python3 - "$H" "$SHA" <<-'EOF'
import json, pathlib, sys

root, want = pathlib.Path(sys.argv[1]), sys.argv[2]

def fail(msg):
    sys.exit(f"measurement: {msg}")

log = root / "home/.SLSsteam.log"
log_text = log.read_text(errors="replace") if log.exists() else ""
attest = root / "attestation.jsonl"
if not attest.exists():
    fail("SLSsteam.so wrote no attestation (did it reach load()?)")
events = [json.loads(line) for line in attest.read_text().splitlines() if line.strip()]
foreign = {e.get("candidate_build_id") for e in events} - {want}
if not events or foreign:
    fail(f"attestation is not from the built SLSsteam.so {want}: {sorted(foreign)}")

modules = {e["module"]: e for e in events if e["event"] == "module-loaded"}
locators = {}
for e in events:
    if e["event"] in ("locator-resolved", "locator-missing"):
        locators[e["locator"]] = e
if not locators:
    fail("no locator events: the pattern scan did not run")

missing = [e for e in locators.values() if e["event"] == "locator-missing"]
required_missing = sorted(e["locator"] for e in missing if not e.get("optional", False))
optional_missing = sorted(e["locator"] for e in missing if e.get("optional", False))

# The catalog is written only after every required pattern resolved; it is
# the binary's own record of which resolved locators are required.
required_resolved = optional_resolved = 0
catalog_locators = 0
for cache in sorted((root / "home/.config/SLSsteam/patterns/local").glob("*/*.cache")):
    for line in cache.read_text().splitlines():
        if line == "required=true":
            required_resolved += 1
        elif line == "required=false":
            optional_resolved += 1
        elif line == "[[locator]]":
            catalog_locators += 1
resolved_events = len(locators) - len(missing)

loaded = "Loaded successfully" in log_text and "Failed to find all patterns" not in log_text
if not required_missing:
    if catalog_locators != resolved_events or required_resolved + optional_resolved != catalog_locators:
        fail(f"catalog lists {catalog_locators} locators, attestation resolved {resolved_events}")
    if not loaded:
        fail("every required pattern resolved but the log has no 'Loaded successfully'")

ipc = [name for name in locators if name.endswith("::RunIPCFrame")]
ipc_missing = [name for name in ipc if locators[name]["event"] == "locator-missing"]
tail = "ready" if len(ipc) >= 6 and not ipc_missing else "pending"

required = required_resolved + len(required_missing) if not required_missing else None
out = {
    "arch": "x86_64",
    "required": str(required) if required is not None else "unknown",
    "resolved": str(required_resolved) if not required_missing else "unknown",
    "pending": str(len(required_missing)),
    "runipcframe_tail": tail,
    "optional_resolved": str(optional_resolved),
    "optional_missing": str(len(optional_missing)),
    "steamclient_build_id": modules.get("steamclient", {}).get("build_id", ""),
    "steamui_build_id": modules.get("steamui", {}).get("build_id", ""),
    "loaded": "yes" if loaded else "no",
    "required_missing": ",".join(required_missing),
    "optional_missing_names": ",".join(optional_missing),
}
for key, value in out.items():
    print(f"{key}={value}")
EOF
)"
m() { printf '%s\n' "$MEASURED" | sed -n "s/^$1=//p"; }
printf '%s\n' "$MEASURED" | sed '/_names=/d; s/^/    /'

# --- 4. agreement with the committed status and the installer gate -----------
echo "==> [4/4] comparing with res/pattern-status.txt at $COMMIT"
committed() {
	printf '%s\n' "$COMMITTED_STATUS" | sed -n "s/^[[:space:]]*$1=//p" | tail -1
}
disagree=""
for key in arch required resolved pending runipcframe_tail; do
	if [ "$(committed "$key")" != "$(m "$key")" ]; then
		disagree="$disagree $key: committed=$(committed "$key") measured=$(m "$key");"
	fi
done
if [ -n "$disagree" ]; then
	echo "    required patterns missing: $(m required_missing)" >&2
	die "res/pattern-status.txt disagrees with what this SLSsteam.so reports when loaded:$disagree refusing to emit a payload"
fi
[ "$(m pending)" = 0 ] && [ "$(m runipcframe_tail)" = ready ] && [ "$(m loaded)" = yes ] ||
	die "the measured pattern table would fail the installer gate (pending=$(m pending) runipcframe_tail=$(m runipcframe_tail)); refusing to emit a payload"

# --- write -------------------------------------------------------------------
P="$WORK/payload"
mkdir -p "$P"
install -m 755 "$SO" "$P/SLSsteam.so"
(cd "$P" && sha256sum SLSsteam.so > SLSsteam.so.sha256)
{
	printf '# Machine-readable state of the x86_64 pattern table of the SLSsteam.so\n'
	printf '# published beside this file. Generated by scripts/build-release-payload.sh\n'
	printf '# in the run that built that library and measured it under LD_AUDIT; do\n'
	printf '# not edit. binary_sha256 is the library these counts describe: refuse them\n'
	printf '# for any other file. optional_missing names features that stay disabled.\n'
	printf '# Optional patterns missing: %s\n' "$(m optional_missing_names | sed 's/,/, /g')"
	for key in arch required resolved pending runipcframe_tail; do
		printf '%s=%s\n' "$key" "$(m "$key")"
	done
	printf 'binary_sha256=%s\n' "$SHA"
	printf 'source_commit=%s\n' "$COMMIT"
	printf 'steamclient_build_id=%s\n' "$(m steamclient_build_id)"
	printf 'steamui_build_id=%s\n' "$(m steamui_build_id)"
	printf 'optional_resolved=%s\n' "$(m optional_resolved)"
	printf 'optional_missing=%s\n' "$(m optional_missing)"
	printf 'glibc_symbol_floor=%s\n' "$glibc_floor"
	printf 'glibcxx_symbol_floor=%s\n' "$glibcxx_floor"
	printf 'cxxabi_symbol_floor=%s\n' "$cxxabi_floor"
} > "$P/pattern-status.txt"

mkdir -p "$(dirname "$OUT")"
rm -rf "$OUT.partial"
cp -r "$P" "$OUT.partial"
rm -rf "$OUT"
mv "$OUT.partial" "$OUT"

echo "==> payload for $COMMIT in $OUT"
(cd "$OUT" && sha256sum SLSsteam.so SLSsteam.so.sha256 pattern-status.txt | sed 's/^/    /')
echo "==> symbol floor: GLIBC_$glibc_floor GLIBCXX_$glibcxx_floor CXXABI_$cxxabi_floor"
echo "==> loads as an LD_AUDIT module on: $load_checked"
echo "==> pattern table: $(m resolved)/$(m required) required, pending=$(m pending), runipcframe_tail=$(m runipcframe_tail), on steamclient $(m steamclient_build_id)"
