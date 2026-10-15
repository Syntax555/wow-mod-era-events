/*
 * mod-era-events: what event creatures cast, and helpers for moving them.
 *
 * Released under the MIT License.
 */

#ifndef MOD_ERA_EVENT_MOBS_H
#define MOD_ERA_EVENT_MOBS_H

#include "Define.h"
#include "Position.h"

#include <vector>

class Creature;
class CreatureAI;

namespace EraEvents
{
    // MovePoint id for "go where the event sent you".
    constexpr uint32 POINT_ARRIVE = 1;

    enum class SpellTarget : uint8
    {
        Victim,
        Self,
        RandomEnemy, // a random player within 40 yards on the threat list
    };

    struct TimedSpell
    {
        uint32 spellId;
        uint32 minMs;
        uint32 maxMs;
        SpellTarget target;
    };

    // For creatures using the generic AI (ScriptName npc_era_event_mob).
    struct MobDef
    {
        std::vector<TimedSpell> spells = {};
        uint32 aura = 0;        // kept on the creature out of combat too
        uint32 spawnVisual = 0; // cast once when it appears
        uint32 deathSpell = 0;
        uint32 enrageSpell = 0;
        uint8 enragePct = 0;
        bool seekEnemies = false; // attacks hostile creatures nearby, not only players

        // Chance per melee hit to put hitAura on a player, unless they have hitAuraBlocker.
        uint32 hitAura = 0;
        uint8 hitAuraChance = 0;
        uint32 hitAuraBlocker = 0;
    };

    void RegisterMob(uint32 entry, MobDef def);
    MobDef const* FindMob(uint32 entry);

    // Walks (or runs) a creature to dest, which becomes its home: after a fight on the way it
    // carries on there instead of going back.
    void SendTo(Creature* creature, Position const& dest, bool run = true);

    void CastTimedSpell(CreatureAI* ai, TimedSpell const& spell);
}

#endif
