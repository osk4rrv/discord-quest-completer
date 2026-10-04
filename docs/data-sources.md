# Data sources

Checked October 4, 2026. Sources are shown in the app's Info report. Catalog membership does not establish current Quest eligibility.

| Source | Use |
| --- | --- |
| [Discord detectable applications](https://discord.com/api/v10/applications/detectable) | Application IDs, store SKUs, icons and Windows executable rules. |
| [markterence mirror](https://markterence.github.io/discord-quest-completer/detectable.json) | Labeled fallback when the primary catalog and cache are unavailable. |
| [Community path index](https://github.com/Sinnisterly/DiscordQuestCompleter/blob/main/dqc/data/overrides.json) | Folder paths in the 55-profile bundled snapshot. Each record includes its source URL. |
| [shizeh executable directory](https://github.com/shizeh/discord-quests-database) | Additional paths for unique title or verified alias matches. |
| [SteamCMD.net](https://www.steamcmd.net/) | Launch metadata for a known Steam ID or unique linked Steam SKU. Community service. |
| [Steam Community](https://steamcommunity.com/actions/SearchApps) and [Steam Store](https://store.steampowered.com/) | Search and game details. |
| [Epic Games Store](https://store.epicgames.com/) | Store search, with public promotions and Discord references as a partial fallback. |

The bundled data is in [`quest-paths.json`](../assets/metadata/quest-paths.json). It contains path facts and attribution. No upstream program or script is downloaded or executed.

## Matching

Store bindings and unique normalized titles resolve Discord IDs. Steam, Discord and Epic identifiers stay separate. Ambiguous matches remain unset.

Paths preserve relative folders. Validation rejects traversal, reserved Windows names, launchers, URI commands and placeholders. Profiles requiring a real Steam demo remain unavailable.

The inspected Discord catalog had 24,560 applications. Fifty of the 55 bundled profile names matched uniquely; unmatched or ambiguous names were left untouched. These counts change with metadata updates. A usable path enables a session attempt, not a guaranteed reward.

## FC 27

Discord application: `1531874756096295054`. Steam app: `4080220`.

The inspected Discord record has no usable Windows executable rule. Steam launch metadata uses an EA launcher URI. The experimental `EA SPORTS FC 27/fc27.exe` profile follows [community reports](https://www.reddit.com/r/DiscordQuests/comments/1wq2m86/ea_sports_fc_27_quest/).

The app discovers registered Steam libraries and creates a temporary helper and `appmanifest_4080220.acf` only when the target folder and manifest do not already exist. Cleanup checks ownership and preserves changed or foreign files. Without a usable library, it uses a local helper folder.

**Latest observed result: 0% Quest progress.** RPC trials received READY and matching activity acknowledgements but did not advance the Quest. A five-minute Standard trial after a full Discord restart also stayed at 0%, as reported from the Quest card. The local-folder check verified helper lifecycle only.

## What the tests prove

The automated checks cover app state, rendering, helper cleanup, RPC transport and Steam ownership using private fixtures. Live RPC has received acknowledgements for Roblox and FC 27. This does not verify Quest credit.

[Rich Presence documentation](https://docs.discord.com/developers/discord-social-sdk/development-guides/setting-rich-presence) describes activity publication. Actual requirements, progress and rewards are in [Discord Quests](https://support.discord.com/hc/en-us/articles/22225719947543-Discord-Quests-FAQ).
