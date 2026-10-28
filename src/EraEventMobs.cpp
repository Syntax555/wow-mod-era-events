/*
 * mod-era-events: the AI every event creature without a script of its own uses.
 *
 * Each event registers what its creatures cast (EraEventMobs.h); this AI runs that list in combat,
 * walks the creature to where the event sends it, and tells the manager when it dies.
 *
 * Released under the MIT License.
 */

#include "EraEventMobs.h"

#include "EraEvents.h"

#include "Creature.h"
#include "MotionMaster.h"
#include "PassiveAI.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"

#include <unordered_map>

namespace EraEvents
{
    namespace
    {
        std::unordered_map<uint32, MobDef>& Defs()
        {
            static std::unordered_map<uint32, MobDef> defs;
            return defs;
        }
    }

    void RegisterMob(uint32 entry, MobDef def)
    {
        Defs()[entry] = std::move(def);
    }

    MobDef const* FindMob(uint32 entry)
    {
        auto itr = Defs().find(entry);
        return itr != Defs().end() ? &itr->second : nullptr;
    }

    void SendTo(Creature* creature, Position const& dest, bool run)
    {
        if (!creature)
            return;

        // Home is where it's headed, so a fight on the way ends with it carrying on there.
        creature->SetHomePosition(dest);
        creature->SetWalk(!run);
        creature->GetMotionMaster()->MovePoint(POINT_ARRIVE, dest);
    }

    void CastTimedSpell(CreatureAI* ai, TimedSpell const& spell)
    {
        switch (spell.target)
        {
            case SpellTarget::Victim:
                ai->DoCastVictim(spell.spellId);
                break;
            case SpellTarget::Self:
                ai->DoCastSelf(spell.spellId);
                break;
            case SpellTarget::RandomEnemy:
                if (Unit* target = ai->SelectTarget(SelectTargetMethod::Random, 0, 40.0f, true))
                    ai->DoCast(target, spell.spellId);
                break;
        }
    }
}

using namespace EraEvents;

struct npc_era_event_mob : public ScriptedAI
{
    npc_era_event_mob(Creature* creature) : ScriptedAI(creature), _def(FindMob(creature->GetEntry())) { }

    void InitializeAI() override
    {
        if (_def && _def->spawnVisual)
            DoCastSelf(_def->spawnVisual, true);

        ScriptedAI::InitializeAI();
    }

    void Reset() override
    {
        scheduler.CancelAll();
        _enraged = false;

        if (_def && _def->aura && !me->HasAura(_def->aura))
            me->AddAura(_def->aura, me);
    }

    void JustEngagedWith(Unit* /*who*/) override
    {
        if (!_def)
            return;

        for (TimedSpell const& spell : _def->spells)
        {
            scheduler.Schedule(Milliseconds(urand(spell.minMs / 2, spell.maxMs)), [this, spell](TaskContext context)
            {
                CastTimedSpell(this, spell);
                context.Repeat(Milliseconds(urand(spell.minMs, spell.maxMs)));
            });
        }
    }

    // Holds the spot it was sent to instead of running back to where it was summoned.
    void MovementInform(uint32 type, uint32 id) override
    {
        if (type == POINT_MOTION_TYPE && id == POINT_ARRIVE)
            me->SetHomePosition(me->GetPosition());
    }

    void DamageDealt(Unit* victim, uint32& /*damage*/, DamageEffectType damageType, SpellSchoolMask /*schoolMask*/) override
    {
        if (!_def || !_def->hitAura || damageType != DIRECT_DAMAGE || !victim || !victim->IsPlayer())
            return;

        if (!IsEventParticipant(victim->ToPlayer(), EVENT_ZOMBIE_INFESTATION))
            return;

        if (victim->HasAura(_def->hitAura) || (_def->hitAuraBlocker && victim->HasAura(_def->hitAuraBlocker)))
            return;

        if (roll_chance_i(_def->hitAuraChance))
            me->AddAura(_def->hitAura, victim);
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (_def && _def->deathSpell)
            DoCastSelf(_def->deathSpell, true);

        NotifyCreatureDied(me);
    }

    void UpdateAI(uint32 diff) override
    {
        // Event armies fight each other, not only players: look for the nearest enemy now and
        // then while idle.
        if (_def && _def->seekEnemies && !me->IsInCombat())
        {
            if (_seekTimer <= diff)
            {
                _seekTimer = 2 * IN_MILLISECONDS;
                if (Unit* target = me->SelectNearestTarget(30.0f))
                    AttackStart(target);
            }
            else
                _seekTimer -= diff;
        }

        if (!UpdateVictim())
            return;

        if (_def && _def->enrageSpell && !_enraged && HealthBelowPct(_def->enragePct))
        {
            _enraged = true;
            DoCastSelf(_def->enrageSpell, true);
        }

        scheduler.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        DoMeleeAttackIfReady();
    }

private:
    MobDef const* _def;
    bool _enraged = false;
    uint32 _seekTimer = 0;
};

// The invisible creature an event spawns everything from. It only speaks.
struct npc_era_anchor : public NullCreatureAI
{
    npc_era_anchor(Creature* creature) : NullCreatureAI(creature) { }
};

void AddEraEventMobScripts()
{
    RegisterCreatureAI(npc_era_event_mob);
    RegisterCreatureAI(npc_era_anchor);
}
