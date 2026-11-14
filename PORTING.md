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

The x86_64 build requires **glibc 2.38** and **GLIBCXX_3.4.32**. Measured, not
assumed: `readelf -V bin/SLSsteam.so`.

| distribution | glibc | runs? |
|---|---|---|
| Ubuntu 24.04 | 2.39 | yes |
| Ubuntu 22.04 | 2.35 | **no** |
| Debian 13 | 2.41 | yes |
| Debian 12 | 2.36 | **no** |
| Fedora 39+ | 2.38 | yes |
| Arch / CachyOS | rolling | yes |

### Why the floor is 2.38 and not 2.34

The source is C++20. Under C++20, glibc's own headers redirect
`strtol`/`strtoul`/`strtoll`/`strtoull` to `__isoc23_strto*`, and those first
appear in glibc 2.38. Nothing in this fork calls `strtol` — it arrives through
`<cstdlib>` in ordinary C++ code — so there is no source-level opt-out. The four
symbols are the whole reason:

```
UND __isoc23_strtoul@GLIBC_2.38
UND __isoc23_strtoll@GLIBC_2.38
UND __isoc23_strtol@GLIBC_2.38
UND __isoc23_strtoull@GLIBC_2.38
```

`GLIBCXX_3.4.32` comes from `std::ios_base::Init` (`ios_base_library_init`), i.e.
gcc 13+ libstdc++.

Building on an older distro does not lower the floor — it just fails to link.
Building on a newer one raises it. That is why the portable container for this
arch is Ubuntu 24.04 and the resulting floor is stated above rather than
pretended away.

### Container build

```sh
scripts/build.sh --portable --arch x86_64   # builds in Ubuntu 24.04
scripts/build.sh --portable                  # i386, still Ubuntu 22.04 (2.34 floor)
```

Two images, because the two arches need genuinely different toolchains:
`scripts/Dockerfile` (22.04, i386 multilib) and `scripts/Dockerfile.x86_64`
(24.04, no multilib).

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
