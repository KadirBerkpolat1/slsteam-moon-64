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
| `pattern-refresh` | only with `--with-helper` |
| `SHA256SUMS` | `sha256sum -c` compatible |
| `abi-floor.txt` | `scripts/abi-floor.sh` report: per-version symbol counts and the symbols forcing each floor |
| `manifest.json` | see below |

`manifest.json` keys: `arch`, `glibc_floor`, `glibcxx_floor`, `cxxabi_floor`
(the maximum over the LD_AUDIT objects), `audit_interface` (the highest LD_AUDIT
version `la_version` accepts; older loaders get their own version back),
`source_commit`, `built_at`, `image`, `image_digest`,
`helper_image`/`helper_image_digest` (with `--with-helper`), `runtime`, and
`files` with per-file floors and sha256.

The script refuses to write the output when the measured `GLIBC`/`GLIBCXX`
floor exceeds the advertised one for the default image (`ADVERTISED_GLIBC` /
`ADVERTISED_GLIBCXX` in the script, or `--max-glibc` / `--max-glibcxx`). For
x86_64 it also loads each object as an LD_AUDIT module under the image's own
`ld.so` before the result leaves the container.

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
