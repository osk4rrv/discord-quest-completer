# Quest Completer

Native C++17 app for Windows 10/11. Browse game metadata, run Standard or Discord RPC sessions, and inspect detection details.

![Appearance](docs/images/appearance.png)

## What's included

- Steam, Epic and Discord search, with pages of 100 and a Standard filter.
- 55 sourced executable folder profiles, plus additional path resolution.
- Owned helper processes with Stop, cleanup and recovery checks.
- RPC acknowledgements, reconnect handling and diagnostics in Info.
- Separate theme and accent palettes, custom colors and DPI-aware rendering.

The catalog lists detectable applications. It does not list the Quests available on your account.

## Before using it

Session timers count local runtime. An RPC acknowledgement means Discord accepted an activity. Neither confirms Quest credit; check progress and claim rewards in Discord.

Select a game, open Info, choose Standard or RPC, then start the session. Stop it from Dashboard or the session window. Closing the app stops its helpers.

## Build

Requires CMake 3.24+, Visual Studio 2022 / Build Tools with **Desktop development with C++**, and the Windows SDK.

```powershell
.\scripts\build.ps1
```

Builds x64 Release, runs four CTest checks and creates `dist/QuestCompleter-Windows-x64.zip`. Add `-Run` to launch it. 

## License

[MIT](LICENSE). Bundled fonts and JSON keep their [own licenses](THIRD_PARTY_NOTICES.md).
