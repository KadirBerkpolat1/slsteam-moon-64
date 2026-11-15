# slsteam-moon

A fork of [SLSsteam](https://github.com/AceSLS/SLSsteam) with extra
protocol handlers, SteamStub support, and a Lua manifest importer.

For the recommended installation method, see
[swwayps/luatools-moon](https://github.com/swwayps/luatools-moon).
For building, see [`docs/BUILDING.md`](docs/BUILDING.md).

This branch also builds for the 64-bit `steamrt64` client — see
[`PORTING.md`](PORTING.md) for the arch matrix, the measured struct layouts,
and the pattern-table work that is still outstanding.

## Credits

Upstream:

- [AceSLS](https://github.com/AceSLS) and contributors —
  the hook framework, protobuf plumbing, ownership patches,
  family-share bypass, and the broader feature set this branch
  builds on.

Reference material:

- [SteamDatabase / SteamAppInfo](https://github.com/SteamDatabase/SteamAppInfo)
  — `appinfo.vdf` v41 format documentation.
- [atom0s / Steamless](https://github.com/atom0s/Steamless) —
  invoked by the wrapper-integration helper.
- [Midrags / SFF / LumaCore](https://github.com/Midrags/SFF) —
  Windows reference port for the `LoadPackage` patch logic.
- [OpenSteamTool](https://github.com/OpenSteam001/OpenSteamTool) —
  some features were ported from or inspired by this project.
  Full credit to its author.

## Licence

AGPL-3.0-only (see [`LICENSE`](LICENSE)), except
[`tools/steamless-bin/`](tools/steamless-bin/), which is under a different licence:
it holds atom0s's [Steamless](https://github.com/atom0s/Steamless) binaries,
redistributed verbatim under CC BY-NC-ND 4.0 (non-commercial, no derivatives).
They are not part of `SLSsteam.so`; SLSsteam runs `Steamless.CLI.exe` as a
separate program, and only for games whose launch options carry `--steamless`.
See [`tools/steamless-bin/NOTICE`](tools/steamless-bin/NOTICE). Third-party
licences for the bundled libraries are in [`docs/LICENSE/`](docs/LICENSE/).

## Support

Open an issue: https://github.com/swwayps/slsteam-moon/issues
