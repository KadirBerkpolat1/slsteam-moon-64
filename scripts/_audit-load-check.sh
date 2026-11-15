#!/bin/sh
# Measures whether this system's ld.so accepts SO as an LD_AUDIT module. Run
# inside the image whose loader is being tested; POSIX sh, as images differ.
#
#   _audit-load-check.sh SO
#
# Prints "glibc=VER rc=RC la_objopen_lookups=N", then whatever ld.so printed.
# rc 0 and no output are not enough: ld.so silently ignores a module whose
# la_version returns 0. N counts ld.so's lookups of la_objopen in SO, which it
# makes only after la_version accepted, whether or not SO defines la_objopen.
# A caller accepts SO only with rc=0, N >= 1 and no ld.so output.

so="$1"
glibc=""
for l in /lib/x86_64-linux-gnu/libc.so.6 /lib64/libc.so.6 /usr/lib64/libc.so.6; do
	[ -x "$l" ] && { glibc=$("$l" 2>&1 | head -1 | sed 's/.*version \([0-9.]*\).*/\1/; s/\.$//'); break; }
done
out=$(LD_AUDIT=$so /bin/true 2>&1); rc=$?
n=$(LD_DEBUG=symbols LD_AUDIT=$so /bin/true 2>&1 |
	awk -v f="lookup in file=$so" 'index($0, "symbol=la_objopen;") && index($0, f) {n++} END {print n+0}')
printf 'glibc=%s rc=%s la_objopen_lookups=%s\n' "$glibc" "$rc" "$n"
printf '%s\n' "$out"
