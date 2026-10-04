# Contributing

Run `scripts/build.ps1` before submitting a change. It builds the app, runs all four checks and packages the release. Use C++17, Windows 10/11 x64 and Visual Studio 2022. Follow `.clang-format`.

## Source layout

- `src/main.cpp`: app state, Win32 events, GPU composition and QA commands.
- `src/interface.inc`, `appearance.inc`, `drawing.inc`, `base.inc`: controls, palettes and drawing.
- `src/services.hpp`, `catalog.inc`, `session.inc`: network services, matching and persistence.
- `src/executable_sources.inc`: supplemental paths, attribution and caches.
- `src/runtime.inc`, `steam_placement.inc`, `runtime_tests.inc`: helper ownership, RPC, Steam placement and integration tests.
- `assets/`, `src/resources.rc`, `third_party/`: embedded resources and dependencies.

## Metadata

Include a source URL and retrieval date for new path facts. Require an unambiguous catalog identity. Do not substitute a Steam ID for a Discord ID. Preserve path validation and ownership checks.

## Diagnostics

QA commands use `%LOCALAPPDATA%\QuestCompleter\qa`. Live diagnostics may publish an activity or start a helper; duration-limited sessions stop automatically.

```powershell
.\dist\QuestCompleter\QuestCompleter.exe --api-test --resolve-executables --search 'Seasons after Fall'
.\dist\QuestCompleter\QuestCompleter.exe --rpc-diagnose --provider discord --search Roblox --diagnostic-seconds 10
```

For bug reports, include the app version, Windows version, title, mode and observed result. Remove private paths and account information from logs. Do not attach tokens or user-data backups.

## Releases

Keep the CMake and Windows resource versions aligned. Push a version tag, such as `v4.6.1`, to build and publish the ZIP and SHA256 file through GitHub Actions. Add user-visible changes to `CHANGELOG.md`.
