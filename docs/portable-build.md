# Portable build and CI recipe

Build `SLSsteam.so` and `library-inject.so` in a pinned old-glibc image so
users never compile anything. The measured floors, and why the effective floor
is higher than the symbol floor, are in [`PORTING.md`](../PORTING.md)
("Distribution floor"). This page covers how to run the builder and the CI
workflow.

## Local

```sh
scripts/build-portable-container.sh                      # x86_64, AlmaLinux 8.10
scripts/build-portable-container.sh --arch i386          # i386, Ubuntu 22.04
scripts/build-portable-container.sh --with-helper        # + bin/pattern-refresh (Ubuntu 22.04)
scripts/build-portable-container.sh --netsock64 FILE     # + netsock64.so from moon64's steamclient_audit64.c
scripts/build.sh --portable --arch x86_64                # same as --with-helper, then copies into bin/
```

Runtimes are tried in this order: podman, docker (reachable daemon), and
bubblewrap with unprivileged user namespaces plus `python3`. Under bubblewrap
the image is pulled straight from the registry by `scripts/_oci-rootfs.py`,
which verifies each layer digest, and the provisioned root filesystem is cached
in `~/.cache/slsteam-moon/portable/<key>`. The key covers the image, the arch
and `scripts/_portable-provision.sh`, so re-runs reuse it. With none of the
three runtimes available the script exits non-zero. It never builds on the host.

The build always uses a clean `git archive` snapshot of `HEAD`, so uncommitted
changes are not included, and the working tree's `obj/`, `bin/` and
`lib/x86_64/libprotobuf-lite.a` are left alone.

## Output contract

`dist/portable/<arch>/` (or `--out DIR`):

| file | content |
|---|---|
| `SLSsteam.so`, `library-inject.so` | the LD_AUDIT objects |
| `netsock64.so` | only with `--netsock64 FILE`: moon64's P2P cert patch, also an LD_AUDIT object |
| `pattern-refresh` | only with `--with-helper` |
| `SHA256SUMS` | `sha256sum -c` compatible |
| `abi-floor.txt` | `scripts/abi-floor.sh` report: per-version symbol counts and the symbols forcing each floor |
| `manifest.json` | see below |

`manifest.json` keys: `arch`, `glibc_floor`, `glibcxx_floor`, `cxxabi_floor`
(the maximum over the LD_AUDIT objects), `audit_interface` (`LAV_CURRENT` in
the image's `<link.h>`, not a check of what any object's `la_version` returns),
`source_commit`, `built_at`, `image`, `image_digest`,
`helper_image`/`helper_image_digest` (with `--with-helper`),
`netsock64_source_sha256` (with `--netsock64`), `runtime`, and `files` with
per-file floors and sha256.

The script refuses to write the output when the measured `GLIBC`/`GLIBCXX`
floor exceeds the advertised one for the default image (`ADVERTISED_GLIBC` /
`ADVERTISED_GLIBCXX` in the script, or `--max-glibc` / `--max-glibcxx`). For
x86_64 it also loads each object as an LD_AUDIT module under the image's own
`ld.so` before the result leaves the container.

`--netsock64 FILE` (x86_64 only) compiles `FILE` in the same image with the
moon64 installer's own command, `gcc -O2 -Wall -Wextra -shared -fPIC -o
netsock64.so steamclient_audit64.c -ldl`. It is a second LD_AUDIT object in the
Steam process, so it is load-checked the same way and counted in the top-level
floors.

## Release payload (`install.sh --prebuilt`)

```sh
scripts/build-release-payload.sh --moon64 ~/path/to/moon64                    # -> dist/release/x86_64/
scripts/build-release-payload.sh --moon64 DIR --moon64-rev REV --out /tmp/staging
```

One command builds the x86_64 payload that moon64's `install.sh --prebuilt`
downloads from a GitHub release, so the user's machine needs no compiler. It
creates no release, and publishing stays a manual step. The output directory
holds exactly five files:

| file | content |
|---|---|
| `SLSsteam.so` | the container build of `HEAD` above |
| `SLSsteam.so.sha256` | `<sha256>  SLSsteam.so` (`sha256sum -c` compatible) |
| `netsock64.so` | moon64's P2P certificate patch, built in the same AlmaLinux 8.10 image from `patches/netsock64/steamclient_audit64.c` at `--moon64-rev` (default: the moon64 checkout's `HEAD`; uncommitted moon64 changes are not included) |
| `netsock64.so.sha256` | `<sha256>  netsock64.so` |
| `pattern-status.txt` | the pattern table measured on that `SLSsteam.so` in the same run, binding both objects by sha256 |

The script refuses to write anything (exit 1, output directory untouched) unless
all of these hold:

- Both container builds succeed.
- `file -b` describes both objects as `ELF 64-bit LSB shared object, x86-64...`,
  the same prefix the installer requires before it saves a prebuilt object.
- Each object's symbol floor is at most `GLIBC_2.17`, `GLIBCXX_3.4.22` and
  `CXXABI_1.3.11`. The working floor in `PORTING.md` depends on these.
- The stock `ubuntu:18.04` image (glibc 2.27, the measured working floor)
  accepts each object as an LD_AUDIT module under its own `ld.so`. It must
  exit 0, print no message, and look up `la_objopen` in the object.
  `--no-load-check` skips this, and the run then says so. For `netsock64.so`
  this is the whole check: it does nothing until `la_objopen` sees a
  `steamclient.so` under `linux64/` or `steamrt64/`, so a pass means `ld.so`
  accepted it, not that it patched anything.
- The library resolves every required pattern when it is LD_AUDIT-loaded into
  an inert process named `steam` that `dlopen`s `steamclient.so` and
  `steamui.so` from `--steam-runtime` (default
  `~/.local/share/Steam/steamrt64`). That process runs in bubblewrap with the
  whole filesystem read-only, no network and an empty `HOME`, so the Steam
  install is only read, never written. The counts come from the library
  itself:
  - its runtime attestation reports every locator as resolved or missing,
    and each event carries the sha256 of the loaded file, which must equal
    the built one;
  - the pattern catalog it writes after a successful scan marks each
    resolved locator `required=true` or `false`.
- The measured `arch`, `required`, `resolved`, `pending` and
  `runipcframe_tail` equal `res/pattern-status.txt` at `HEAD`, and pass the
  installer's gate (`pending=0`, `runipcframe_tail=ready`).

### What binds `pattern-status.txt` to both objects

`pattern-status.txt` is an output of the run that built both objects, never a
copy of the checked-in file. It has exactly one `binary_sha256=` line, the
sha256 of the `SLSsteam.so` it was measured on, and exactly one
`netsock64_sha256=` line, the sha256 of the `netsock64.so` built in the same
run. A consumer must:

1. compute the sha256 of the downloaded `SLSsteam.so` and `netsock64.so`;
2. if the `.sha256` files are published, compare their first fields;
3. require exactly one `binary_sha256=` line equal to the `SLSsteam.so` sha256
   and exactly one `netsock64_sha256=` line equal to the `netsock64.so` sha256,
   and refuse otherwise;
4. only then gate on `arch=x86_64`, `required`, `resolved`, `pending=0` and
   `runipcframe_tail=ready`.

Step 3 is what makes the file evidence about these two objects. Without it, a
stale status file can vouch for a library it was never measured on, or travel
with a cert patch from another build.

Informational keys, which a consumer may log and must not require:

| key | meaning |
|---|---|
| `source_commit` | the commit that was built |
| `netsock64_source_commit` | the moon64 commit `netsock64.so` was built from |
| `steamclient_build_id`, `steamui_build_id` | GNU build-ids of the Steam libraries the counts were measured against |
| `optional_resolved`, `optional_missing` | optional locators; each missing one disables only its own feature |
| `glibc_symbol_floor`, `glibcxx_symbol_floor`, `cxxabi_symbol_floor` | `readelf -V` floor of the library |
| `netsock64_glibc_symbol_floor` | `readelf -V` floor of `netsock64.so` (C only, so it needs no `GLIBCXX`/`CXXABI`) |

### `netsock64.so` floor

`netsock64.so` needs `libc.so.6` (20 symbols, all `GLIBC_2.2.5`) and
`libdl.so.2` (linked by the installer's `-ldl`, no symbol used). It exports
only `la_version` and `la_objopen`, `dlopen`s nothing and takes nothing from
the Steam runtime. Its floor is therefore set by the LD_AUDIT interface
version, not by symbols.

AlmaLinux 8's headers define `LAV_CURRENT 2` (glibc 2.28 with interface v2
backported). An `la_version` that returns `LAV_CURRENT` unconditionally asks
every loader for v2, and every loader below glibc 2.34 in the matrix below,
except AlmaLinux's, knows only v1. A local `gcc` build never shows this,
because the local headers match the local loader. A prebuilt object does.

Measured on the objects built by `--netsock64` in `almalinux:8.10` (gcc 13.3.1),
each LD_AUDIT-loaded into `/bin/true` under the stock image's own `ld.so`, with
the acceptance check above:

| image | glibc | moon64 `f3ec02b`, `return LAV_CURRENT` | moon64 `17382e5`, negotiated |
|---|---|---|---|
| `centos:7` | 2.17 | segfault, rc 139 | loads |
| `ubuntu:16.04` | 2.23 | segfault, rc 139 | loads |
| `debian:9` | 2.24 | segfault, rc 139 | loads |
| `ubuntu:18.04` | 2.27 | segfault, rc 139 | loads |
| `debian:10` | 2.28 | segfault, rc 139 | loads |
| `almalinux:8.10` | 2.28 | loads (vendor v2 backport) | loads |
| `ubuntu:20.04`, `debian:11` | 2.31 | `ld.so` assertion in `_dl_debug_vdprintf`, rc 127 | loads |
| `fedora:33`, `fedora:34` | 2.32, 2.33 | "requires version 2 (maximum supported version 1); ignored", rc 0: Steam starts without the cert patch | loads |
| `fedora:35`, `ubuntu:22.04`, `debian:12` | 2.34-2.36 | loads | loads |

Every failure is the interface-version request. No image lacked a symbol the
object needs. The negotiated object (sha256 `f53a832d…`, from `17382e5`)
loads on every image down to glibc 2.17, below `SLSsteam.so`'s 2.27. A host
build is not an alternative: built on glibc 2.44 with gcc 16, `sscanf` binds to
`__isoc23_sscanf@GLIBC_2.38`, while the AlmaLinux build binds it to
`sscanf@GLIBC_2.2.5`.

What this does not establish:

- Neither object has been run inside a real Steam client by this script. The
  pattern counts come from an inert stand-in process, and `netsock64.so` is
  only shown to be accepted by `ld.so`.
- The payload is unsigned. The checksums and the binding catch corruption and
  mismatched files, not a compromised release: anyone who can replace release
  assets can replace all five files together. Only a signature checked against
  a key pinned in the installer would defend against that.

## CI workflow (not enabled)

`.gitignore` ignores `.github` on purpose, which keeps CI off forks. A workflow
file committed there is never tracked, so GitHub never runs it. **To enable this,
the owner must first remove `.github` from `.gitignore`**, then save the block
below as `.github/workflows/release-portable.yml`.

It builds both arches in the pinned images on `ubuntu-latest` and uploads
workflow artifacts only. It creates no GitHub release and publishes nothing.
UNVERIFIED: the YAML parses, but the workflow has never run on GitHub. It
assumes the runner image ships rootless podman.

```yaml
name: release-portable

on:
  workflow_dispatch:
  push:
    tags: ['v*']

permissions:
  contents: read

jobs:
  build:
    name: build (${{ matrix.arch }})
    runs-on: ubuntu-latest
    strategy:
      fail-fast: false
      matrix:
        include:
          - arch: x86_64
            helper: --with-helper
          - arch: i386
            helper: ''
    steps:
      - uses: actions/checkout@v4

      - name: Build in the pinned old-glibc image
        run: >-
          scripts/build-portable-container.sh
          --runtime podman
          --arch '${{ matrix.arch }}'
          ${{ matrix.helper }}
          --out 'dist/portable/${{ matrix.arch }}'

      - name: Verify checksums
        working-directory: dist/portable/${{ matrix.arch }}
        run: sha256sum -c SHA256SUMS && cat manifest.json

      - uses: actions/upload-artifact@v4
        with:
          name: slsteam-moon-${{ matrix.arch }}
          path: dist/portable/${{ matrix.arch }}/
          if-no-files-found: error
```
