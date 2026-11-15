#!/usr/bin/env bash
# Measure the glibc / libstdc++ versions an ELF object requires at load time.
#
# The required set is read from the version-needs table (readelf -V), which is
# exactly what ld.so checks before it will map the object; objdump -T then names
# the symbols bound to each version, so the report says *why* a floor is what it
# is, not just what it is.
#
# Usage:
#   scripts/abi-floor.sh FILE...          per-version counts + forcing symbols
#   scripts/abi-floor.sh --floor NS FILE  print only the highest NS version
#                                         (NS: GLIBC, GLIBCXX, CXXABI, GCC);
#                                         prints nothing if FILE needs none

set -euo pipefail

READELF="${READELF:-readelf}"
OBJDUMP="${OBJDUMP:-objdump}"
NAMESPACES=(GLIBC GLIBCXX CXXABI)

usage() { sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 2; }

# "NS_x.y.z" lines, one per required version, from .gnu.version_r.
required_versions() {
	"$READELF" -V -W "$1" | awk '
		/^Version needs section/ { inneed = 1; next }
		/^Version (symbols|definition) section/ { inneed = 0 }
		inneed && $0 ~ /Name: / { for (i = 1; i <= NF; i++) if ($i == "Name:") print $(i + 1) }
	' | sort -u
}

# "NS_x.y.z symbol" lines for every undefined dynamic symbol.
versioned_imports() {
	"$OBJDUMP" -T -w "$1" | awk '
		/\*UND\*/ {
			for (i = 1; i <= NF; i++) {
				v = $i; gsub(/[()]/, "", v)
				if (v ~ /^[A-Z]+_[0-9]/) { print v, $NF; break }
			}
		}
	' | sort -u
}

floor_of() {
	local ns="$1" file="$2"
	required_versions "$file" | awk -v ns="$ns" '
		index($0, ns "_") == 1 { v = substr($0, length(ns) + 2); if (v ~ /^[0-9.]+$/) print v }
	' | sort -V | tail -1
}

report() {
	local file="$1" imports ns top
	imports="$(versioned_imports "$file")"
	echo "== $file"
	for ns in "${NAMESPACES[@]}"; do
		top="$(floor_of "$ns" "$file")"
		[ -n "$top" ] || { echo "$ns: none"; continue; }
		printf '%s\n' "$imports" | awk -v ns="$ns" '
			index($1, ns "_") == 1 && substr($1, length(ns) + 2) ~ /^[0-9.]+$/ { n[$1]++ }
			END { for (v in n) printf "  %-16s %5d\n", v, n[v] }
		' | sort -k1,1V
		printf '%s floor: %s_%s, forced by:\n' "$ns" "$ns" "$top"
		printf '%s\n' "$imports" | awk -v want="${ns}_$top" '$1 == want { print "    " $2 }'
	done
}

[ $# -gt 0 ] || usage
command -v "$READELF" >/dev/null 2>&1 || { echo "abi-floor: $READELF not found" >&2; exit 1; }
command -v "$OBJDUMP" >/dev/null 2>&1 || { echo "abi-floor: $OBJDUMP not found" >&2; exit 1; }

if [ "$1" = --floor ]; then
	[ $# -eq 3 ] || usage
	floor_of "$2" "$3"
	exit 0
fi
case "$1" in -h|--help) usage ;; esac

for f in "$@"; do
	[ -s "$f" ] || { echo "abi-floor: $f is missing or empty" >&2; exit 1; }
	report "$f"
done
