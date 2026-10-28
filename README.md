# Era Events

Timed outdoor encounters for each character's earned Individual Progression
tier in [AzerothCore Playerbots Portable](https://github.com/Syntax555/mod-playerbots-portable).
Players, SelfBot characters and bots see and fight an event only at its exact
tier and in its zone. A party leader or another character cannot unlock it.
This native module requires the assembly's matching Individual Progression and
core; no additional client files are required.

## Install

Use the assembly's [Latest release and setup guide](https://github.com/Syntax555/mod-playerbots-portable#quick-start)
for a playable Windows server. Era Events is included and enabled.
For native development, follow its
[build guide](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/building.md)
and [dependency lock](https://github.com/Syntax555/mod-playerbots-portable/blob/main/versions.lock.json),
which selects a tested commit of this fork's `main` branch and matching dependencies.
The native module directory is `mod-era-events`; rebuild worldserver after
native changes. Its world SQL is applied by the assembly's database update path.

## Encounters and limits

| Event | Earned tier | Encounter |
| --- | --- | --- |
| Gates of Ahn'Qiraj | `4` | Qiraji waves at the Scarab Wall in Silithus |
| Scourge Invasion | `6` | City attacks or necropolis encounters |
| Legion Incursion | `7` | Demon waves near Vanilla towns |
| Zombie Infestation | `12` | Plague crates, zombies, infection and Argent Healers in Stormwind or Orgrimmar |

The strict default Individual Progression chain must be active. Event tiers
respect the realm ceiling without erasing saved milestones; custom or disabled
chains run no events. Players travel normally. Each event has an independent
120–240 minute schedule, a 10-minute warning and a 30-minute duration (AQ: 45).
Restarting worldserver resets schedules and running encounters.

Scheduled starts require one real non-GM player at the event tier in the
portable profile; bots participate but do not meet that minimum. `.era next`
shows your tier's event and timer. GMs can use `.era list`,
`.era start <aq|si|legion|zombie>` and `.era stop <aq|si|legion|zombie>`.
Manual starts skip the timer and player minimum, while retaining availability
and earned-tier rules.

Realm-wide AQ phasing and automatic gold, reputation or mailed-item rewards
are refused, even if configured or requested by a GM. Merely standing in a
zone earns no payout. Normal creature loot retains kill and loot eligibility
checks; no progression, money or items are granted for attendance. The separate
core Scourge Invasion (game event 17) remains disabled by Individual Progression.
Complete encounters and bot tactics still need in-game validation. See
[world events](https://github.com/Syntax555/mod-playerbots-portable/blob/main/docs/world-events.md).

## Configuration

Edit `configs/modules/mod_era_events.conf` with the portable server stopped.
All options are in [conf/mod_era_events.conf.dist](conf/mod_era_events.conf.dist).

| Setting | Portable default | Native template |
| --- | --- | --- |
| `EraEvents.Enable` | `1` | `1` |
| `EraEvents.MinRealPlayers` | `1` | `2` |
| `EraEvents.RetryMinutes` | `5` | `5` |
| `EraEvents.<Event>.Enable` | `1` | `1` |
| `EraEvents.<Event>.IntervalMin` / `IntervalMax` | `120` / `240` | Same |
| `EraEvents.<Event>.WarningMinutes` | `10` | Same |
| `EraEvents.<Event>.DurationMinutes` | `30` (AQ: `45`) | Same |

`<Event>` is `AQWar`, `ScourgeInvasion`, `LegionIncursion` or `ZombieInfestation`.
The portable profile sets `AQWar.RealmWideMinutes` and every event's
`RewardGold`, `RewardReputation`, `RewardItem` and `RewardItemCount` to `0`.
The native template retains upstream reward and realm-wide examples; this fork
clamps all those payouts and realm-wide duration to zero at runtime.

## Source, contributions and license

Author: [buildthehomelab](https://github.com/buildthehomelab); maintained in
[Syntax555/wow-mod-era-events](https://github.com/Syntax555/wow-mod-era-events).
Built for [Individual Progression](https://github.com/ZhengPeiRu21/mod-individual-progression)
by **ZhengPeiRu21**. The
[imported upstream README](https://github.com/buildthehomelab/wow-mod-era-events/blob/cf02786aceb531df76ff065d8be7a1973344cfb1/README.md)
describes the original events; this fork's earned-play restrictions apply here.

Read [AGENTS.md](AGENTS.md) and the assembly's
[contribution workflow](https://github.com/Syntax555/mod-playerbots-portable/blob/main/CONTRIBUTING.md).
Portable integration CI checks earned guards and startup data; native builds
and release integration run in the assembly. Licensed under [MIT](LICENSE).
Copyright (c) 2026 buildthehomelab; original notices and history are retained.
