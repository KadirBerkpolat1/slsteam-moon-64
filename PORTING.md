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

Ten files. Each one is a place where 32-bit and 64-bit genuinely differ, not a
stylistic change.

| file | change |
|---|---|
| `src/utils.hpp` | added `<cstdint>` — `uint8_t`/`uint32_t` were undeclared. **This also breaks the pristine upstream tree on gcc 16**, so it is a straight bug fix, not a port artifact. |
| `src/main.cpp` | `auditSymbind<SymT>` template behind `la_symbind32` and `la_symbind64` |
| `src/sdk/CSteamEngine.hpp` | `CServerPipe` 64-bit layout + `plausibleServerPipe()` gate |
| `src/sdk/CPackageInfo.hpp` | `CUtlMemory` / `CUtlVector` / `PackageInfo` offsets by pointer width |
| `src/feats/apps.cpp` | `size_t` → `uint32_t` to match the header and the hooked signature |
| `src/feats/libraryremoval.cpp` | explicitly unsupported on x86_64 (x86-32 instruction decoder) |
| `Makefile` | `ARCH` matrix, `obj/$(ARCH)`, arch stamp |
| `scripts/build.sh`, `scripts/_build-host.sh` | `--arch` and arch-aware dev-lib sniffing |
| `src/hooks.cpp` | uses `plausibleServerPipe()` before keying the process map |

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
| `CNetPacket` | 0x20 | 0x20 |

`CServerPipe`'s 64-bit offsets come from AceSLS's reverse engineering, not from
the 2026-10-22 client on this machine, so `plausibleServerPipe()` checks the pid
is in a human range before anything keys off it. `PackageInfo`'s fixed-width
leading fields mean `Status`/`SHA_1_Hash` do not move at all; only the two
vectors shift. `CNetPacket` is a raw `uint8_t[0x20]` overlay and is
arch-independent.

### libraryremoval is x86-32 only

`deriveOwnershipOffset()` decodes `8B /r` (mod=01) from the x86-32 SteamUI
instruction stream. On x86_64 that decoder rejects every instruction, so
`setup()` reports the capability unavailable and installs no detour. The
feature is off, not wrong.

## Distribution floor — read this before shipping a binary

The x86_64 container build ships against its symbol floor. Do not advertise
anything lower than the measured numbers below.

| floor | x86_64 (`scripts/build.sh --portable --arch x86_64`) |
|---|---|
| symbol floor (`readelf -V`) | `GLIBC_2.17`, `GLIBCXX_3.4.22`, `CXXABI_1.3.11` |
| loader gate (`la_version`) | none: an old-glibc loader negotiates interface v1 — see "LD_AUDIT interface version" |

`la_version` used to keep the effective floor at glibc 2.34: the audit objects
always asked for interface v2, and loaders that only know v1 abort. They now
answer with the version the loader offers, so an old-glibc loader negotiates
instead of aborting, and the AlmaLinux build is no longer loader-gated at 2.34.

What remains above the symbol floor is the host's own toolchain, not the
loader. A host build stays at `GLIBC_2.38`. The i386 container build stays near
2.34 (see "i386"). The AlmaLinux-built objects have not been re-run through the
load matrix since the fix, so 2.17 is the measured symbol floor, not a measured
working floor.

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
loader. Every hook the objects define exists in interface v1. Those runs used
`library-inject.so` and a pure-C probe, not the AlmaLinux-built `SLSsteam.so`.

Negotiation does not lower a host build's floor: its symbol versions
(`GLIBC_2.38`, `GLIBCXX_3.4.32`) stop `dlmopen` on older glibc before
`la_version` is called, and `ld.so` then ignores the module (rc 0).

Not verified: none of this was run inside the Steam client. The load test
proves that the loader resolves every version need and accepts the audit
module. It does not exercise patching. The host also needs a `libstdc++.so.6`
of at least `GLIBCXX_3.4.22` (gcc 6). Whether Steam's runtime supplies a newer
one in front of the host's was not measured.

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

The pre-fix abort table above is x86_64. On i386 only the post-fix probe on
`debian:11` (2.31) was measured.

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

## Pattern table — the remaining work

**This is why the port is not usable yet.**

The fork carries 61 byte signatures; 37 are required (a missing required
pattern makes `Patterns::init()` fail and SLSsteam aborts the entire injection,
leaving Steam unpatched). Measured against this machine's libraries:

| library | signatures with 8+ bytes | unique matches |
|---|---|---|
| `ubuntu12_32/steamclient.so` + `steamui.so` | 57 | **53** |
| `steamrt64/steamclient.so` + `steamui.so` | 57 | **0** |

The 32-bit column proves the scanner is correct; the 64-bit zero is real. Every
signature has to be re-derived, not copied.

AceSLS's 64-bit build supplies 7 usable seeds (`TraceIPC`,
`CSteamEngine::GetServerPipe`, `SetAppIdForCurrentPipe`, `ProcessIPCFrame`,
`CUser::CheckAppOwnership`, `IClientUser::UpdateAppOwnershipTicket`,
`CWebSocketConnection::BBuildAndAsyncSendFrame`). All 7 hit
`steamrt64/steamclient.so` exactly once inside the executable PT_LOAD.

**23 required patterns remain**, plus 6 `RunIPCFrame` dispatchers whose
resolution tail is also arch-specific: the `IpcFrame` scanner looks for
`E8 ?? ?? ?? ?? 8B 85 ?? ?? ?? ?? 83 C4 10 3D` (32-bit `add esp,0x10`);
`steamrt64` contains neither that nor the `48 83 C4 10` form.

There is no symbol-table shortcut. `steamrt64/steamclient.so` keeps a real
`.symtab` but only 483 named functions, all third-party (`Breakpad_*`, OpenSSL,
tinyxml), and 30 RTTI records, all tinyxml/SteamThreadTools. Steam's own
classes are stripped in both arches. `CreateInterface` *is* exported, so vtable
indices — which are arch-independent — remain an alternative route for the
interface methods.

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
P2P session, no FakeAppIds path, nothing runtime. The pattern table above is the
blocker, and until it is solved the binary loads and then declines to patch.

## Upstream

```sh
git fetch upstream
git rebase upstream/slsteam-moon
```

The 32-bit tree tracks `swwayps/slsteam-moon` closely; keep the ported changes
as a small, reviewable commit range on top rather than editing upstream files
in place wherever a `#if defined(__x86_64__)` guard will do.
