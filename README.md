# Era Events

An [AzerothCore](https://www.azerothcore.org/) (WotLK 3.3.5a) module that brings back the iconic
world events of each era as **timed live events**, for servers running
[mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression).

With individual progression, every character is in its own era. IP already sets the scene for
each one (the War Effort, the outdoor AQ war, the Scourge Invasion's necropolises, the Legion at
the Dark Portal), but nothing ever *happens* there. This module adds the moments:

| Event | Era (IP tier) | What happens |
| --- | --- | --- |
| **Gates of Ahn'Qiraj** | 4: the gong is rung | The Qiraji swarm storms out of the Scarab Wall in waves against a line of Kaldorei infantry. Merithra, Arygos and Caelestrasz join in one after another and strafe the swarm, then Lieutenant General Nokhor leads the last push. **Banging the gong** starts it for the whole realm: see [The Ahn'Qiraj War](#the-ahnqiraj-war). |
| **Scourge Invasion** | 6: after AQ, before Naxxramas | Takes turns: a Pallid Horror or Patchwork Terror marches through Stormwind or Undercity dragging Flameshockers along, or the Herald of the Lich King raises three rare elites and a stream of minions beside a necropolis. |
| **Legion Incursion** | 7: after Naxxramas, before the Dark Portal | A fel portal tears open outside Gadgetzan, Everlook, Cenarion Hold, Nethergarde Keep or Light's Hope Chapel. Demons march on the town in waves, a Dreadlord leads the middle one and a Pit Commander comes through last. |
| **Zombie Infestation** | 12: the end of TBC, before Northrend | The 3.0.2 plague in Stormwind or Orgrimmar. Plagued grain crates, rising dead, **infection** ("You're Infected!"), Argent Healers who cure it, and if you're not cured in time you **turn into a zombie** with the original zombie form and its action bar. Your groan infects the living around you. |

## Shared, but only with your era

- **Only your era sees it.** Everything an event spawns sits in a phase of its own, and a character
  is phased in only while standing in the event's zone *and* at the event's tier. Someone at a
  different stage in the same city sees nothing and isn't attacked.
- **Needs company.** A scheduled event only starts when at least **2 real players** at that tier
  are online (`EraEvents.MinRealPlayers`). Playerbots and GMs don't count towards that, but
  playerbots at the tier are phased in and fight alongside you. The gong is the one exception: it
  starts the AQ war even for a single player.
- **Announced.** Everyone at the tier gets a warning 10 minutes before (on screen and in chat)
  saying where, then a message when it starts and when it ends.
- **Rewarded.** Win it and everyone phased in gets gold, reputation (Cenarion Circle for AQ, Argent
  Dawn for the rest) and, for the Scourge Invasion, Necrotic Runes. All configurable. Items go
  by mail when bags are full.

Each event runs on its own schedule: a random 2 to 4 hours between chances by default. When
it's due but too few players are online, it checks again every 5 minutes.

## The Ahn'Qiraj War

On the original realms the gong started the Ten Hour War. Here **the moment anyone completes
Bang a Gong!** the Gates of Ahn'Qiraj run for **two hours, for the whole realm**, with no notice
and no minimum number of players:

- **Everyone online is told** who rang the gong, on screen and in chat.
- **Every era fights at the Scarab Wall.** Whoever comes to Silithus is in the battle, whatever
  their tier.
- **The gate doesn't fall quiet.** When Nokhor dies that assault pays out to everyone there, the
  Kaldorei line is brought back up to strength, and five minutes later the swarm comes again. That
  repeats until the two hours are over.
- **The hives are open to all.** mod-individual-progression has a war of its own that only
  characters between the gong and *Chaos and Destruction* can see: the three Colossi at the hives,
  the Anubisath and General Rajaxx at the gate, and Qiraji crystal camps in Silithus (level 60)
  and Darkshore (level 16 to 20). For the two hours everyone sees them, so friends of any era and
  level can help. The ringer still has to kill the Colossi to move on: the war ending doesn't do
  that for them.

A second gong while the war runs starts the two hours over. `.era next` tells anyone how long is
left. `EraEvents.AQWar.RealmWideMinutes` sets the length, and 0 turns the war off: the gong then
starts a plain 45 minute run for the ringer's era, as the scheduled ones are.

## Commands

| Command | Who | What it does |
| --- | --- | --- |
| `.era next` | everyone | Your era's event, whether it's running, and when the next chance comes. |
| `.era list` | GM | Every event, its tier, state and timer, and how many real players are at its tier. |
| `.era start <aq\|si\|legion\|zombie> [realm]` | GM | Start an event now, skipping the notice and the player count. `.era start aq realm` starts the Ahn'Qiraj War as the gong does. |
| `.era stop <aq\|si\|legion\|zombie>` | GM | End a running event and despawn it. |

To test an event, put a character at its tier with `.ip set <tier>` (IP's command), go to the zone and
`.era start <event>`.

## Requirements

- [AzerothCore](https://www.azerothcore.org/) wotlk (master) and a WoW 3.3.5a (12340) client.
- [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) with
  `IndividualProgression.Enable = 1`. Events are tied to its progression tiers; with IP off no
  era has players. The module reads IP's hidden progression quests and config but doesn't link
  against it.
- No client patch. [mod-playerbots](https://github.com/mod-playerbots/mod-playerbots) is optional:
  bots at the tier are phased in and fight alongside you.

## Installation

Clone it into your AzerothCore `modules` folder **as `mod-era-events`**, without the repo's `wow-`
prefix. AzerothCore finds the module's entry point from the folder name.

```bash
cd <azerothcore>/modules
git clone https://github.com/buildthehomelab/wow-mod-era-events.git mod-era-events
```

Rebuild the worldserver, then copy `conf/mod_era_events.conf.dist` to your config folder as
`mod_era_events.conf`. The world database updates add the event creatures and the phase auras on
the next start.

No client patch is needed: everything uses spells and models the 3.3.5 client already has.

## Configuration

See `conf/mod_era_events.conf.dist`.

| Option | Default | What it does |
| --- | --- | --- |
| `EraEvents.Enable` | 1 | Master switch. |
| `EraEvents.MinRealPlayers` | 2 | Real players at the tier who must be online for a scheduled event to start. |
| `EraEvents.RetryMinutes` | 5 | How often a due event checks again while waiting for players. |

Each event (`AQWar`, `ScourgeInvasion`, `LegionIncursion`, `ZombieInfestation`) has its own set,
for example `EraEvents.ScourgeInvasion.Enable`:

| Option | Default | What it does |
| --- | --- | --- |
| `.Enable` | 1 | 0 never runs it; GMs can't start it either. |
| `.IntervalMin` / `.IntervalMax` | 120 / 240 | Minutes between one event and the next chance, picked at random. |
| `.WarningMinutes` | 10 | Notice before it starts. 0 starts it straight away. |
| `.DurationMinutes` | 30 (AQ 45) | How long players have before the event gives up. |
| `.RealmWideMinutes` | AQ only: 120 | How long the Ahn'Qiraj War lasts once the gong is rung. 0 turns it off. |
| `.RewardGold` | 15-50 | Gold for everyone taking part when it's won. |
| `.RewardFaction` / `.RewardReputation` | 609 or 529 / 150-250 | Reputation reward. Faction 0 for none. |
| `.RewardItem` / `.RewardItemCount` | 22484 x5 for SI, else none | Item reward. |

The module reads `IndividualProgression.Enable` and `IndividualProgression.ProgressionLimit` from
IP's config: with IP off no era has players, and an event whose tier is beyond the progression
limit never runs.

## How it works

- A character's era is read from IP's hidden progression quests (66000 + tier), the same way IP
  reads it. The module doesn't link against IP.
- Each event owns a phase bit IP doesn't use (IP uses 16-21, this module 22-25) and a server-side
  aura that adds it, like IP's own "Phase Shift" auras. The two stack, so IP's phasing in the same
  zones keeps working.
- An event spawns an invisible anchor and summons everything else from it, so it all inherits the
  event's phase. The anchor keeps the area loaded and updating while nobody is near.
- The Ahn'Qiraj War drops the tier check for its two hours, and adds a second aura with IP's own
  AQ war phase (65536) in the areas where IP gives it to a character of that era. IP's spawns
  aren't touched or copied: everyone is simply let into the phase they are already in.
- The creatures are copies of Blizzard's, with this module's scripts: the originals' own scripts
  write the core's global Scourge Invasion state or run the Ahn'Qiraj quest flashback.

## IDs used

- `creature_template` 9500800-9500809 (Scourge Invasion), 9500810-9500818 (AQ), 9500820-9500827
  (Legion), 9500850-9500853 (Zombie)
- `gameobject_template` 9500854 (plagued grain crate)
- `spell_dbc` 90120-90123 (phase auras), 90124 (mod-individual-progression's AQ war phase, for
  the Ahn'Qiraj War)
- `spell_script_names` 56560 (Beckoning Groan, on the zombie form's bar)
- phase bits 22-25 (4194304, 8388608, 16777216, 33554432)

## Notes

- The Scourge Invasion here is its own, not the core's world event (game event 17). Leave that one
  off; mod-individual-progression disables it too.
- Zombie players get the invasion's faction for as long as they're turned, so the dead leave them
  alone. City guards don't: turning in the middle of Stormwind is dangerous.
- Events don't survive a restart: a running event is simply gone, and the schedule starts over.

## Troubleshooting

- **An event never starts by itself:** a scheduled event needs at least 2 real players at its tier
  online (`EraEvents.MinRealPlayers`), and bots and GMs don't count. It checks again every 5
  minutes. Also check `EraEvents.Enable`, the event's own `.Enable`, and that its tier isn't past
  `IndividualProgression.ProgressionLimit`.
- **You can't see a running event:** only characters standing in the event's zone and at its tier
  are phased in. Check your tier with `.era next`, and set a test character's with `.ip set <tier>`.
- **Testing an event:** a GM can run `.era start <aq|si|legion|zombie>`, which skips the notice and
  the player count, and `.era stop` to end it.
- **Testing the Ahn'Qiraj War:** `.era start aq realm` starts it as the gong does, for any
  character, and `.era stop aq` ends it. Stand in Silithus: the battle is at the Scarab Wall and
  the Colossi are at the hives.
- **Two Scourge Invasions at once:** the core's game event 17 is on. Leave it off.

## Credits

The module is built for [mod-individual-progression](https://github.com/ZhengPeiRu21/mod-individual-progression) by ZhengPeiRu21 and reads its progression state.

Author: [buildthehomelab](https://github.com/buildthehomelab)

## License

MIT, see [LICENSE](LICENSE).
