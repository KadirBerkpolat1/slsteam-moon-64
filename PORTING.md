# x86_64 port

This branch builds `slsteam-moon` for Steam's 64-bit `steamrt64` client in
addition to the original 32-bit `ubuntu12_32` one. The 32-bit build is
unchanged and remains the default.

## Build

```sh
make                     # i386 (Steam ubuntu12_32) — unchanged default
make ARCH=x86_64         # x86_64 (Steam steamrt64)
make clean               # removes obj/, bin/, dist/ for BOTH arches
```

`ARCH` defaults to `i386` on x86_64 hosts so a bare `make` keeps producing what
it always produced. `CROSS_HOST=X86_64` / `CROSS_HOST=AArch64` still work as
before.

Outputs are the same three files for both arches — `bin/SLSsteam.so`,
`bin/library-inject.so`, `bin/pattern-refresh`. `bin/` is shared, so every
arch-dependent target depends on `obj/$(ARCH)/.arch`; switching `ARCH`
invalidates the stamp and forces a relink. Without that stamp `make
ARCH=x86_64 && make` would leave a 64-bit `SLSsteam.so` in place and the next
32-bit client launch would try to load ELF64 through `LD_AUDIT`.

## Dependencies

`lib/x86_64/` carries the 64-bit static libraries:

| library | source | size |
|---|---|---|
| `liblibmem.a` | AceSLS/SLSsteam `lib/x86_64/` | 15784140 |
| `libyaml-cpp.a` | AceSLS/SLSsteam `lib/x86_64/` | 1056990 |
| `libprotobuf-lite.a` | built here from the vendored 3.15.3 sources | 1800318 |

protobuf-lite is built natively rather than copied because every `.pb.h` in
`src/sdk/protobufs/` derives from `MessageLite`, so the 29-object lite subset is
the whole requirement and it must match the vendored headers exactly. The build
rule is `lib/x86_64/libprotobuf-lite.a` in the Makefile.

LuaJIT is **not** linked: this fork has no `Plugins` subsystem. It would only
be needed if AceSLS's `src/lua.cpp` / `LuaBridge` were ported in.

## What changed for 64-bit

Twenty files. Each one is a place where 32-bit and 64-bit genuinely differ, not
a stylistic change.

| file | change |
|---|---|
| `src/utils.hpp` | added `<cstdint>` — `uint8_t`/`uint32_t` were undeclared. **This also breaks the pristine upstream tree on gcc 16**, so it is a straight bug fix, not a port artifact. |
| `src/main.cpp` | `auditSymbind<SymT>` template behind `la_symbind32` and `la_symbind64` |
| `src/sdk/CSteamEngine.hpp` | `CServerPipe` 64-bit layout + `plausibleServerPipe()` gate |
| `src/sdk/CSteamEngine.cpp` | `getLocalClientCompat()` reads the 7-byte REX.W `lea` on x86_64 and decodes `CUser::m_CompatManager` at 0x4A58 |
| `src/sdk/CPackageInfo.hpp` | `CUtlMemory` / `CUtlVector` / `PackageInfo` offsets by pointer width |
| `src/sdk/CNetPacket.hpp` | x86_64 field layout (0x28 overlay) and an x86_64-only shift probe — see "Struct layouts" |
| `src/feats/apps.cpp` | `size_t` → `uint32_t` to match the header and the hooked signature |
| `src/feats/libraryremoval.cpp` | x86_64 path: `deriveOwnershipOffset64()`, no i386 PIC-thunk repair |
| `src/feats/libraryremoval_policy.hpp` | `deriveOwnershipOffset64()`, the REX-aware ownership-read decoder — see "libraryremoval" |
| `src/feats/compatlive.hpp` | `CompatLive::decodeManagerOffset64()` for the 7-byte `lea` with a disp32 |
| `src/feats/manifestbind.cpp` | `CUtlVector<DepotEntry>` capacity/count offsets 0x08/0x10 on x86_64 |
| `src/memhlp.cpp`, `src/memhlp.hpp` | `fixPICThunkCall()` reads the `E8` rel32 operand as a signed 32-bit value instead of a pointer-width one, and refuses non-executable addresses |
| `src/patterns.cpp` | x86_64 signature arms for the required patterns, the six formerly-optional locators and the five SteamUI locators |
| `src/hooks.cpp` | uses `plausibleServerPipe()` before keying the process map |
| `Makefile` | `ARCH` matrix, `obj/$(ARCH)`, arch stamp; x86_64 test defaults point at `steamrt64` |
| `scripts/build.sh`, `scripts/_build-host.sh` | `--arch` and arch-aware dev-lib sniffing |
| `scripts/_build-portable.sh`, `scripts/build-portable-container.sh` | x86_64 `--portable` builds in AlmaLinux 8.10; i386 keeps the Ubuntu 22.04 `scripts/Dockerfile` |

The matching `tools/test_*.cpp` files carry x86_64 arms or arch-gated layout
assertions for the same code.

### `la_symbind64`

This is the change that matters most. The fork hooks
`execv`/`execvp`/`execve`/`execvpe`/`posix_spawn`/`posix_spawnp` through
`la_symbind32` to rewrite Steam's hard-coded `--remote-debugging-port=8080` and
publish the real port to the Lumen sidecar contract file
(`$HOME/.local/share/Lumen/cef_port`). glibc calls `la_symbind64` for 64-bit link
maps, so without it that rewrite never installs on `steamrt64` and Lumen falls
back to 8080 — where any dev server already listening makes the sidecar
unreachable.

Both entries now share one `auditSymbind<SymT>` body so they cannot drift.

### Struct layouts

Measured with g++ on this host, not guessed:

| struct / field | i386 | x86_64 |
|---|---|---|
| `CUtlMemory<uint32_t>` | 12 | 16 |
| `CUtlVector<uint32_t>` | 16 | 24 |
| `CServerPipe` | 0x60 | 0x30 |
| `CServerPipe::pipeHandle` | 0x8 | 0x10 |
| `CServerPipe::pid` | 0x14 | 0x20 |
| `PackageInfo::Status` | 0x18 | 0x18 |
| `PackageInfo::SHA_1_Hash` | 0x1C | 0x1C |
| `PackageInfo::AppIdVec` | 0x38 | 0x40 |
| `PackageInfo::DepotIdVec` | 0x48 | 0x58 |
| `CNetPacket` | 0x20 | 0x28 |

`CServerPipe`'s 64-bit offsets come from AceSLS's reverse engineering, not from
the 2026-10-22 client on this machine, so `plausibleServerPipe()` checks the pid
is in a human range before anything keys off it. `PackageInfo`'s fixed-width
leading fields mean `Status`/`SHA_1_Hash` do not move at all; only the two
vectors shift.

`CNetPacket` is a raw byte overlay whose field offsets depend on the arch
(`src/sdk/CNetPacket.hpp`). x86_64 carries 64-bit pointers: `m_pubData` at
+0x10, `m_cubData` at +0x18 and `m_pubOriginalData` at +0x20, 0x28 in total
(measured on steamrt64 `steamclient.so` build `a57ea463`). i386 keeps the classic
+0x4 / +0x8 / +0x10 layout, 0x20 in total. `detectLayout()` probes field shifts
`{0, 8}` on i386, where a beta client moved every field by +8, but only `{0}`
under `__x86_64__`.

### libraryremoval: the ownership offset differs by arch

LibraryRemoval runs on both arches. `setup()` recovers the ownership-flags
offset from the instruction SteamUI's `FillInAppOverview` uses to read it, never
from a layout constant, because `CSteamApp` widens with the pointer size:
**0x18 on i386, 0x1c on x86_64**.

- i386: `deriveOwnershipOffset()` accepts only `8B /r` with mod=01, reg=eax and
  base ecx or edx (`8B 41 disp8` / `8B 42 disp8`).
- x86_64: `deriveOwnershipOffset64()` accepts an optional REX prefix that may
  only select the base (W=1 or R=1 is rejected: `(instruction[0] & 0x0C) != 0`),
  then `8B /r` with mod=01 and reg=edx. It refuses a SIB byte (rm=100) and
  `[rbp/r13+disp8]` (rm=101), sign-extends the disp8 and rejects a zero or
  negative displacement. On steamrt64 `steamui.so` build `57d58821` the read is
  `41 8B 56 1C` (`mov edx,[r14+0x1c]`).

Both decoders also reject a displacement above 0x100 or one that is not 4-byte
aligned. Measured live with the LD_AUDIT harness against the real steamrt64
`steamclient.so` and `steamui.so`: both detours install and the log reports
`LibraryRemoval: SteamUI queue ready (ownership offset=0x1c)`.

Keep both decoders strict. The feature writes zero into whatever field the
offset resolves to, so a decoder that accepted the read of a neighbouring field
would silently zero that field instead. An unrecognised read must keep failing
closed: `setup()` then reports the capability unavailable and installs no
detour.

## Distribution floor — read this before shipping a binary

There are two floors per arch, and they differ. Advertise the **working** one.

| floor | x86_64 (`scripts/build.sh --portable --arch x86_64`) | i386 (`scripts/build.sh --portable`) |
|---|---|---|
| symbol floor (`readelf -V`) | `GLIBC_2.17`, `GLIBCXX_3.4.22`, `CXXABI_1.3.11` | `GLIBC_2.34`, `GLIBCXX_3.4.30`, `CXXABI_1.3.13` |
| loader gate (`la_version`) | none: answers the version `ld.so` offers (v1 or v2) | none: answers the version `ld.so` offers (v1 or v2) |
| **working floor, measured** | **glibc 2.27** (Ubuntu 18.04) with libstdc++ from **gcc 7+** | **glibc 2.35** (Ubuntu 22.04) with libstdc++ from **gcc 12+** |
| what stops the next-older system | `CXXABI_1.3.11` (libstdc++): `exception_ptr(void*)`, `__cxa_init_primary_exception` | `GLIBCXX_3.4.30` (libstdc++): `condition_variable::wait` |

On x86_64, glibc never blocks: the working floor is set by libstdc++, and
`SLSsteam.so` loads wherever libstdc++ provides `CXXABI_1.3.11` (gcc 7). The
tested images with glibc 2.17, 2.23 and 2.24 all ship an older libstdc++, so
2.27 is the lowest glibc on which a load was measured. That is not a claim that
2.17-2.26 systems fail on glibc. On i386 the working floor is one step above the
symbol floor's glibc: Fedora 35 (glibc 2.34) ships a gcc 11 libstdc++ without
`GLIBCXX_3.4.30`.

### Working floor: load matrix

Method, run against `0a02dfb` (the negotiated `la_version`):

- Built by `scripts/build-portable-container.sh` (x86_64 in `almalinux:8.10`,
  i386 in `ubuntu:22.04`) from a `git archive` snapshot.
  - x86_64 `SLSsteam.so` sha256: `f4bc31ff…`
  - i386 `SLSsteam.so` sha256: `e90b1aca…`
- Each object is loaded with `LD_AUDIT` into a trivial executable under each
  stock image's own `ld.so`, glibc and `libstdc++` (registry rootfs entered with
  bubblewrap).
  - x86_64 executable: the image's `/bin/true`.
  - i386 executable: a `_start`-only binary that imports nothing newer than
    `GLIBC_2.0`.
  - i386 runtime: every image is the amd64 rootfs plus the distro's own i386
    `libc` and `libstdc++` packages, from its own (or archive) repositories.
- Accepted means three things:
  - exit 0;
  - no `ld.so` message;
  - `ld.so` looked up `la_objopen` in the module. It does that only after
    accepting `la_version`. A rejected module prints `...; ignored.` and still
    exits 0, so the exit code alone proves nothing.
- Every rejection was cross-checked statically: the object's version needs
  against the version definitions of that image's libraries. The two agree on
  every pair, and the static pass lists every missing need, whereas `ld.so`
  reports only the first.
- Controls on each image:
  - a module hard-wired to request interface v2 reproduces the pre-fix
    failures;
  - a host build (`GLIBC_2.38`) is rejected everywhere.

Failure modes:

- **(a) la_version rejection:** the loader refuses the requested audit
  interface version.
- **(b) symbol-version need:** the image lacks a GLIBC, GLIBCXX or CXXABI
  version the object was linked against.
- **(c) missing runtime library:** a library the plugin `dlopen`s later is
  absent.

x86_64, `SLSsteam.so`:

| image | glibc | max GLIBCXX | result | rc | mode | blocking needs (all of them) |
|---|---|---|---|---|---|---|
| `centos:7` | 2.17 | 3.4.19 | **rejected**, process **segfaults** | 139 | (b) | `GLIBCXX_3.4.20`/`.21`/`.22`, `CXXABI_1.3.8`/`.9`/`.11` (libstdc++ only; no GLIBC need missing) |
| `ubuntu:16.04` | 2.23 | 3.4.21 | **rejected**, process **segfaults** | 139 | (b) | `CXXABI_1.3.11` (`exception_ptr(void*)`, `__cxa_init_primary_exception`), `GLIBCXX_3.4.22` (`std::thread::_M_start_thread`) |
| `debian:9` | 2.24 | 3.4.22 | **rejected**, process **segfaults** | 139 | (b) | `CXXABI_1.3.11` only |
| `ubuntu:18.04` | 2.27 | 3.4.25 | loads | 0 | — | — |
| `debian:10` | 2.28 | 3.4.25 | loads | 0 | — | — |
| `almalinux:8.10` | 2.28 | 3.4.25 | loads | 0 | — | — |
| `ubuntu:20.04` | 2.31 | 3.4.28 | loads | 0 | — | — |
| `debian:11` | 2.31 | 3.4.28 | loads | 0 | — | — |
| `fedora:33` | 2.32 | 3.4.28 | loads | 0 | — | — |
| `fedora:34` | 2.33 | 3.4.29 | loads | 0 | — | — |
| `fedora:35` | 2.34 | 3.4.29 | loads | 0 | — | — |
| `ubuntu:22.04` | 2.35 | 3.4.30 | loads | 0 | — | — |
| `debian:12` | 2.36 | 3.4.30 | loads | 0 | — | — |

x86_64 `library-inject.so` loads on every image from `ubuntu:16.04` up. On
`centos:7` it is rejected on `GLIBCXX_3.4.20` (rc 0, ignored).

Failure mode (a) is gone: no image rejected either object on the audit
interface. Note that the v2 control aborts or segfaults on `ubuntu:18.04`,
`debian:10`, `ubuntu:20.04`, `debian:11` (rc 127/139), and is ignored as
"requires version 2" on `fedora:33`/`34`. Our objects load on all of them.

Failure mode (c) cannot block loading. No object has `libcurl` or `libcrypto`
in `DT_NEEDED`:

- x86_64 `SLSsteam.so`: `libpthread`, `libdl`, `libstdc++`, `libm`, `libgcc_s`,
  `libc` and `ld-linux`.
- i386 `SLSsteam.so`: the same without `libpthread`/`libdl`.

They are `dlopen`ed when a feature runs (see "Runtime dependencies"). Several
stock container images lack them: `libcurl.so.4` is absent from the `ubuntu`
and `debian` images, and `libcrypto` from `ubuntu:20.04` and `debian:12`.
That is a packaging question for minimal systems, not part of the floor.

A rejection on the oldest loaders tested is not benign. On `centos:7` (2.17),
`ubuntu:16.04` (2.23) and `debian:9` (2.24), `ld.so` printed the version error
and then the process segfaulted (rc 139). By inference, not measured, a Steam
client started with `LD_AUDIT` on such a system would crash rather than run
unpatched.

Not verified:

- None of this ran inside the Steam client. The matrix proves that `ld.so`
  resolves every version need and accepts the audit module; it does not
  exercise patching.
- Whether Steam's runtime puts its own `libstdc++.so.6` in front of the host's
  for the audited process was not measured. The libstdc++ rows above use the
  image's copy.

### Symbol floor by build image

Method: build in the image with `scripts/build-portable-container.sh`, then
`scripts/abi-floor.sh` (the version-needs table from `readelf -V`, with symbols
attributed from `objdump -T`). Same source, three toolchains:

| built in | GLIBC | GLIBCXX | CXXABI | forced by |
|---|---|---|---|---|
| host: gcc 16.2.1, glibc 2.44 (`make ARCH=x86_64`) | 2.38 | 3.4.32 | 1.3.15 | `__isoc23_strto{l,ll,ul,ull}`, `ios_base_library_init`, `__cxa_call_terminate` |
| `ubuntu:22.04`: gcc 11, glibc 2.35 | 2.34 | 3.4.30 | 1.3.13 | `dlopen`/`pthread_*` (13 symbols), `condition_variable::wait` |
| `almalinux:8.10`: gcc-toolset-13, glibc 2.28 | **2.17** | **3.4.22** | **1.3.11** | `clock_gettime`, `secure_getenv`; `std::thread::_M_start_thread` |
| `debian:11`: gcc 10, glibc 2.31 | — | — | — | does not compile: gcc 10's libstdc++ has no `std::hash<std::filesystem::path>` (`src/process.cpp`) |

`library-inject.so` from the AlmaLinux build: `GLIBC_2.17`, `GLIBCXX_3.4.21`,
`CXXABI_1.3.9`.

The 2.38 host floor is an artefact of the build host, not of the source:
`__isoc23_strto*` are emitted only when compiling C++20 against glibc >= 2.38
headers, and `ios_base_library_init` only by gcc 13+ libstdc++.
`lib/x86_64/libprotobuf-lite.a` also carries those references, but the Makefile
rebuilds it from the vendored sources on every clean build, so it follows the
build image too. AlmaLinux 8's gcc-toolset links symbols its stock gcc 8
`libstdc++.so.6` lacks from `libstdc++_nonshared.a`, which is why GLIBCXX stays
at 3.4.22.

### LD_AUDIT interface version

`la_version()` (`src/main.cpp`, `tools/library-inject/main.cpp`) answers with
the version `ld.so` offers, capped at `LAV_CURRENT`. It used to return
`LAV_CURRENT` unconditionally. The host, `ubuntu:22.04` and `almalinux:8.10`
headers all define `LAV_CURRENT 2` (AlmaLinux 8's glibc 2.28 backports
interface v2), so every shipped build asked for v2. A loader that only knows v1
rejected them, and on the versions below it **aborted the process** instead of
ignoring the module.

Measured before the fix, by loading the AlmaLinux-built objects with `LD_AUDIT`
into `/bin/true` under each stock image's own `ld.so` and `libstdc++`:

| image | glibc | result |
|---|---|---|
| `ubuntu:20.04` | 2.31 | ld.so assertion in `_dl_debug_vdprintf`, rc 127 |
| `debian:11` | 2.31 | ld.so assertion in `_dl_debug_vdprintf`, rc 127 |
| `fedora:33` | 2.32 | "requires version 2 (maximum supported version 1)", then ld.so assertion in `unload_audit_module`, rc 127 |
| `fedora:34` | 2.33 | same as fedora:33, rc 127 |
| `fedora:35` | 2.34 | loads |
| `ubuntu:22.04` | 2.35 | loads |
| `debian:12` | 2.36 | loads |
| `almalinux:8.10` | 2.28 | loads (vendor backport of interface v2) |

A trivial audit module compiled in `debian:11` itself loads there, so the abort
was the version request, not the toolchain. That is what held the effective
floor at 2.34.

After the fix, measured on real distro loaders (rootfs under bwrap):
`la_objopen`/`la_objsearch` fire on `fedora:34` (2.33), `ubuntu:20.04` and i386
`debian:11` (2.31), and `la_symbind` still fires for a `-z now` executable on
2.31, 2.33 and 2.34, so a BIND_NOW client is still interposed under a v1
loader. Every hook the objects define exists in interface v1. The load matrix
above repeats the acceptance check with the container-built `SLSsteam.so`
itself.

Negotiation does not lower a host build's floor: its symbol versions
(`GLIBC_2.38`, `GLIBCXX_3.4.32`) stop `dlmopen` on older glibc before
`la_version` is called, and `ld.so` then ignores the module (rc 0). In the
matrix a host build was rejected on all eight original images. The first
missing need it reported ranged from `GLIBC_2.29` (`almalinux:8.10`) to
`GLIBC_2.38` (`fedora:35`, `ubuntu:22.04`, `debian:12`).

### i386

`scripts/build.sh --portable` (Ubuntu 22.04) measures `GLIBC_2.34`,
`GLIBCXX_3.4.30`, `CXXABI_1.3.13` for `SLSsteam.so` (same forcing symbols as
x86_64 on 22.04), plus the `GCC_7.0.0` (`libgcc_s.so.1`) and
`GLIBC_ABI_GNU_TLS` (`libc.so.6`) version needs, and `GLIBC_2.4`,
`GLIBCXX_3.4.29` for `library-inject.so`. Both negotiate the audit interface
too, so the floor near 2.34 is the Ubuntu 22.04 toolchain's, not the loader's.
The AlmaLinux treatment is not available for i386 yet:

- gcc-toolset-13 on AlmaLinux 8 ships no 32-bit libstdc++ headers
  (`bits/c++config.h: No such file or directory`, measured);
- the prebuilt `lib/libyaml-cpp.a` (i386) references `__libc_single_threaded`
  (27 times, `nm -u`), a `GLIBC_2.32` symbol, so linking against glibc < 2.32
  fails until it is rebuilt from source.

i386 load matrix (same method, run under the distro's own i386
`/lib/ld-linux.so.2`):

| image | glibc | max GLIBCXX | `SLSsteam.so` | blocking needs (all of them) | `library-inject.so` |
|---|---|---|---|---|---|
| `almalinux:8.10` | 2.28 | 3.4.25 | rejected, rc 0 | `GLIBC_2.29` (`log2`), `2.32`, `2.33`, `2.34`; `GLIBCXX_3.4.26`, `.29`, `.30`; `CXXABI_1.3.13` | rejected (`GLIBCXX_3.4.26`, `.29`) |
| `ubuntu:20.04` | 2.31 | 3.4.28 | rejected, rc 0 | `GLIBC_2.32` (`__libc_single_threaded`), `2.33` (`stat`/`fstat`/`lstat`), `2.34` (13 `dlopen`/`pthread_*`); `GLIBCXX_3.4.29`, `.30`; `CXXABI_1.3.13` | rejected (`GLIBCXX_3.4.29`) |
| `debian:11` | 2.31 | 3.4.28 | rejected, rc 0 | same as `ubuntu:20.04` | rejected (`GLIBCXX_3.4.29`) |
| `fedora:33` | 2.32 | 3.4.28 | rejected, rc 0 | `GLIBC_2.33`, `2.34`; `GLIBCXX_3.4.29`, `.30`; `CXXABI_1.3.13` | rejected (`GLIBCXX_3.4.29`) |
| `fedora:34` | 2.33 | 3.4.29 | rejected, rc 0 | `GLIBC_2.34`; `GLIBCXX_3.4.30` | loads |
| `fedora:35` | 2.34 | 3.4.29 | **rejected**, rc 0 | `GLIBCXX_3.4.30` only (`condition_variable::wait`) | loads |
| `ubuntu:22.04` | 2.35 | 3.4.30 | loads | — | loads |
| `debian:12` | 2.36 | 3.4.30 | loads | — | loads |

Every i386 rejection is mode (b). None is a `la_version` rejection, and none
crashed the process. Lowering the i386 floor needs an older build image, which
the two bullets above currently block. It does not need a loader change.

### Container build

```sh
scripts/build.sh --portable --arch x86_64   # AlmaLinux 8.10 via build-portable-container.sh
scripts/build.sh --portable                  # i386, Ubuntu 22.04 (scripts/Dockerfile)
scripts/build-portable-container.sh --arch x86_64   # artifacts + manifest only, bin/ untouched
```

`scripts/build-portable-container.sh` uses podman, docker or bubblewrap, in that
order, and exits non-zero rather than falling back to a host build. It builds a
clean snapshot of `HEAD` and refuses when the measured floor exceeds the
advertised one. Its `manifest.json` records the floors, the audit interface
version and the image digest. The x86_64 `--portable` path builds
`bin/pattern-refresh` in a second pass in Ubuntu 22.04, because that helper
needs libcurl >= 7.66 (`curl_multi_poll`) and OpenSSL 3, which AlmaLinux 8
lacks. It is still gated by `scripts/check-pattern-refresh-abi.sh`. CI recipe:
`docs/portable-build.md`.

## Runtime dependencies

`SLSsteam.so` links only `libstdc++`, `libm`, `libgcc_s`, `libc`, `ld-linux`.
Notably it does **not** link `libcurl` or `libdbus`:

- **libcurl** is loaded at runtime with `dlopen("libcurl.so.4")`
  (`src/curl.cpp:22`). A 64-bit `libcurl.so.4` must exist on the host; Steam does
  not ship one in `steamrt64/`. Every mainstream distro has it via the base curl
  package, and the fork degrades to "libcurl unavailable" rather than failing to
  load if it is missing.
- **libdbus** is not used at all. AceSLS's binary needs `libdbus-1.so.3`; this
  fork does not, because the DBus-using feature set was never in it. That
  removes an install step the release binary required.

## Pattern table — current position

Required patterns are complete on x86_64. `res/pattern-status.txt`, which
moon64's install gate reads, records `required=38 resolved=38 pending=0
runipcframe_tail=ready`, measured against `steamrt64/steamclient.so` build
`a57ea463`.

Method: every x86_64 signature is re-derived against the live `steamrt64`
libraries, not copied from i386, and then confirmed in a live LD_AUDIT harness.
The built `SLSsteam.so` is audited into an inert process named `steam` that
`dlopen`s the real `steamclient.so` and `steamui.so` under an isolated `HOME`,
and SLSsteam's own `Required pattern` / `Optional pattern` log lines are read
back.

The six formerly-optional locators now resolve on x86_64, each with two
independent proofs on `steamclient.so` build `a57ea463`:

| locator | proof 1 | proof 2 |
|---|---|---|
| `CCMInterface::RecvPkt` | only function referencing `"CCMInterface::RecvPkt"`, `"../clientdll/cminterface.cpp"` and the heartbeat-response string | calls `CProtoBufMsgBase::InitFromPacket`, `CJobMgr::BRouteMsgToJob`, `CSteamEngine::PostCallback` and the CNetMsg factory (17 callers, matching i386) |
| `CUser::ProcessPendingLicenseUpdates` | iterates `CUser::m_vecPendingLicenses` (+0x2588) and per entry calls the package-keyed lookup at `0x102e680` (`GetOrAddPackageData` role, receiver `CUser+0x1440`), matching i386; `CAppInfoCache::GetOrAddAppData` is a different callee, `0x102e360`, called at `0x185132c`, `0x18513a9` and `0x1851547` | single external caller, matching i386; calls the `LicensesUpdated_t` notifier |
| `CDepotDownloadMgr::BuildDepotDependency` | only function carrying `"Failed to initialize depot %u, manifest %llu (%s)"`, `"count >= 0"` and `"../public/tier1/utlvector.h"` | exactly 4 callers × 2 call sites (flag 0 and flag 1), matching i386's 4 callers |
| `CUser::m_CompatManager` | the called constructor writes the vtable whose RTTI name is `14CCompatManager` | inside `CUser::CUser`, `lea rax,[rbx+0x4a58]` hands the `CUser` to that constructor |
| `ParentalSignatureCheck` | directly follows the RSA signature-verification call inside the "Validating parental settings" function | exact `84 C0 75 24` guarding `"Parental settings signature did not verify for account ID %u"` (i386: `84 C0 75 27`) |
| `CAPIJob::GetPlayerStats` | only function referencing `"Player.GetUserStats#1"` | single external caller carrying `"Bad GameID passed to RequestUserStats()"` and the `UserStatsReceived_t` strings, matching i386 |

`make ARCH=x86_64 test-optional-locators` checks that all six resolve uniquely
in the installed libraries. The five SteamUI locators
(`AppControllerRunFrame`, `GetAppByID`, `MarkAppChange`,
`BuildCompleteAppOverviewChange`, `OwnershipFlagsReference`) resolve on
`steamui.so` build `57d58821`, with ownership offset 0x1c (see "libraryremoval").

Harness run on `bcec565` (`bin/SLSsteam.so` BuildID `9213390e`, the source
merged to `main` in `1f5bf16`): 0 `Required pattern` misses,
`LibraryRemoval: SteamUI queue ready (ownership offset=0x1c)`,
`ReconcilePin: hooked EvaluateConfigChanges ... ctx_depots=+0xc0`,
`Loaded successfully`, and **1 optional pattern that does not resolve**:
`CDepotDownloadMgr::OnChunkUnpacked[regparm3]`. The same harness on `9691886`,
before the second batch, logged 12.

### Optional locators ported in the second batch

Each was re-derived on `steamclient.so` build `a57ea463` with two independent
proofs, recorded next to its signature in `src/patterns.cpp` and in its commit
message. `make ARCH=x86_64 test-optional-locators` checks that they resolve
uniquely in the installed libraries.

| pattern | what it enables on x86_64 |
|---|---|
| `CUser::MarkLicenseAsChanged` | the mark capability of `LicenseRefreshPolicy::decide()` (`src/feats/license_refresh_policy.hpp`); with `ProcessPendingLicenseUpdates` from the first batch and the `GetOrAddAppData` guard below, every input it checks before returning `Process` instead of `Defer` now resolves, so by source the restart-less licence refresh path is no longer gated off (not observed running, see below) |
| `CUser::NotifyLicensesUpdated` | the `LicensesUpdated_t` broadcast used by the deferred-refresh fallback and the cold-cache reconcile (`src/feats/packagepatch.cpp`) |
| `CAppInfoCache::GetOrAddAppData`, `SkipFlagReference`, `ShaReference` | the unresolved-app skip guard (`hkGetOrAddAppData`, `src/feats/appinfostate.cpp`); the x86_64 field offsets come from `AppDataLayout::derive64` (`src/feats/appdata_layout.hpp`) |
| `CAppInfoCache::ThreadedReadFromDisk` | live appinfo reloads (`AppInfoState::reloadFromDisk`) |
| `CDepotDownloadMgr::ProcessDepotManifest`, `PrepareDepotDownload` | the offline / provider-down manifest GID fallback (`redirectGid`, `src/feats/manifestbind.cpp`) |
| `CDepotDownloadMgr::EvaluateConfigChanges` | ReconcilePin (`src/feats/reconcilepin.cpp`); context depot vector at `+0xc0` (i386 `+0x90`) |
| `ParentalSettingsReceived` | local parental unlock (`Parental::setup()`, `src/feats/parental.cpp`) |
| `CDepotDownloadMgr::OnChunkUnpacked[cdecl]` | live auto-quarantine of undecryptable DLC depots (`src/feats/depotquarantine.cpp`) |

`CDepotDownloadMgr::OnChunkUnpacked[regparm3]` stays unresolved by design.
x86_64 SysV has no cdecl/regparm(3) split, so its x86_64 signature is empty;
the `[cdecl]` locator hooks the one SysV implementation, and
`DepotQuarantine::setup()` is active when either variant installs. The
"dependent feature disabled" line it logs costs nothing on x86_64.

What the harness does not show: none of these paths ran there. The inert
process has no logged-in user, so `PackagePatch` logs `runtime refresh
generation 1 deferred (package 0 is not ready)` and restart-less licence
refresh is still unobserved.

Ownership and DLC at boot never depended on the optional locators. `PackagePatch::setup()` needs
only `LoadPackage` and `CUtlMemory::Grow` (`src/feats/packagepatch.cpp:910-950`),
both required and resolved, and logs "boot path remains available" when the
runtime refresh capability is incomplete. `CUser::CheckAppOwnership` and
`CUser::GetSubscribedApps` are required and resolve. The DLC vtable hooks
(`src/hooks.cpp:907-912`) are placed from the required
`IClientApps::RunIPCFrame` (`src/hooks.cpp:1512`). By source, the boot path
for Package 0 injection is intact and every input it requires resolves; it
has not been observed in a running client.

The resolved `CCMInterface::RecvPkt` hook does **not** stand in for licence
refresh. On `kEMsgClientLicenseList`, `src/feats/cmrecv.cpp:76-95` only calls
`Achievements::recvMessage` and `ManifestDonor::onLicenseList`; it never
injects Package 0 and never broadcasts `LicensesUpdated_t`.

There is no symbol-table shortcut. `steamrt64/steamclient.so` keeps a real
`.symtab` but only 483 named functions, all third-party (`Breakpad_*`, OpenSSL,
tinyxml), and 30 RTTI records, all tinyxml/SteamThreadTools. Steam's own
classes are stripped in both arches. `CreateInterface` *is* exported, so vtable
indices — which are arch-independent — remain an alternative route for the
interface methods.

Limit: all of this comes from the LD_AUDIT harness. It proves the signatures
resolve and the hooks install against the real libraries, not that the patches
behave correctly inside a running Steam client.

## Verification status

Run on Linux 7.3.4, gcc 16.2.1, glibc 2.44, Steam client builds 2026-10-22.

| check | command | result |
|---|---|---|
| x86_64 syntax, 69 TUs | `g++ -fsyntax-only -DX86_64 ...` | 69/69 |
| i386 syntax, 69 TUs | `g++ -fsyntax-only -m32 ...` | 69/69 |
| x86_64 link | `make ARCH=x86_64` | exit 0, `ELF 64-bit` |
| i386 link | `make` | exit 0, `ELF 32-bit` |
| audit exports (64-bit) | `nm -D bin/SLSsteam.so` | `la_objopen`, `la_preinit`, `la_symbind32`, `la_symbind64`, `la_version` |
| undefined symbols | `ldd -r bin/SLSsteam.so` | 0 |
| glibc ceiling | `readelf -V` | `GLIBC_2.38` <= host 2.44 |
| arch stamp | `make ARCH=x86_64 && make` | relinks both artifacts |

**Not verified:** the client has never been launched against this build. No
P2P session, no FakeAppIds path, no licence refresh, nothing runtime. In the
LD_AUDIT harness the required patterns are complete (0 misses), hook setup
completes (`Loaded successfully`), and the only optional miss is the
i386-only `OnChunkUnpacked[regparm3]` (see "Optional locators ported in the
second batch").

## Upstream

```sh
git fetch upstream
git rebase upstream/slsteam-moon
```

The 32-bit tree tracks `swwayps/slsteam-moon` closely; keep the ported changes
as a small, reviewable commit range on top rather than editing upstream files
in place wherever a `#if defined(__x86_64__)` guard will do.
