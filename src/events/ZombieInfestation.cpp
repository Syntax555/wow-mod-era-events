/*
 * mod-era-events: Zombie Infestation (tier 12, the end of the Burning Crusade, before Northrend).
 *
 * The 3.0.2 plague, in the capital of whichever side has more players of the era online. Plagued
 * grain crates turn up in the Trade District or the Valley of Strength and the dead rise around
 * them. A zombie's bite can infect you ("You're Infected!"); an Argent Healer standing by cleanses
 * it, but if the infection runs its course you turn ("You're a Zombie!"): the zombie form with its
 * own action bar, and your groan infects the living around you. Smash every crate and put down
 * every zombie to win.
 *
 * Both player auras and the zombie form are the original event's spells from the client's data.
 *
 * Released under the MIT License.
 */

#include "EraEventMobs.h"
#include "EraEvents.h"

#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuras.h"
#include "SpellScript.h"
#include "SpellScriptLoader.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "WorldSession.h"

#include <unordered_map>

namespace EraEvents
{
    namespace
    {
        // creature_template / gameobject_template copies from data/sql/db-world.
        enum Entries : uint32
        {
            NPC_ZOMBIE_ANCHOR     = 9500850, // invisible trigger
            NPC_PLAGUE_ZOMBIE     = 9500851,
            NPC_PLAGUED_RESIDENT  = 9500852,
            NPC_ARGENT_HEALER     = 9500853,
            GO_PLAGUED_CRATE      = 9500854, // Plagued Grain Crate, made clickable
        };

        enum Spells : uint32
        {
            SPELL_YOURE_INFECTED    = 43958,
            SPELL_YOURE_A_ZOMBIE    = 43869, // zombie form, its action bar and zombie vision
            SPELL_ZOMBIE_FACTION    = 44281, // "Zombie Tech Test": the invasion's faction, so the dead leave you be
            SPELL_CURE_INFECTION    = 48309, // the Argent Healer's cast
            SPELL_BECKONING_GROAN   = 56560, // on the zombie bar
            SPELL_INFECTED_BITE     = 7367,
            SPELL_MINION_SPAWN_IN   = 28234,
        };

        constexpr uint32 MAP_EASTERN_KINGDOMS = 0;
        constexpr uint32 MAP_KALIMDOR = 1;

        struct City
        {
            char const* name;
            char const* where;
            uint32 mapId;
            uint32 zoneId;
            Position center;
        };

        std::array<City, 2> const CITIES = {{
            { "Stormwind", "the Trade District",    MAP_EASTERN_KINGDOMS, 1519, { -8826.53f, 614.393f, 94.4371f, 1.05716f } },
            { "Orgrimmar", "the Valley of Strength", MAP_KALIMDOR,        1637, { 1603.48f, -4449.95f, 8.36028f, 2.3911f } },
        }};

        constexpr uint32 CRATES = 6;
        constexpr uint32 HEALERS = 2;
        constexpr uint32 RAISE_MS = 30 * IN_MILLISECONDS;
        constexpr uint32 MAX_ZOMBIES = 25;
        constexpr uint32 CHECK_MS = IN_MILLISECONDS;
        constexpr uint32 INFECTION_MS = 60 * IN_MILLISECONDS;
        constexpr uint32 ZOMBIE_MS = 3 * MINUTE * IN_MILLISECONDS;
        constexpr float HEAL_RANGE = 8.0f;
        constexpr float GROAN_RANGE = 12.0f;

        void TellPlayer(Player* player, std::string const& text)
        {
            if (WorldSession* session = player->GetSession())
                ChatHandler(session).SendSysMessage(text);
        }

        void SetTimed(Aura* aura, uint32 ms)
        {
            if (!aura)
                return;
            aura->SetMaxDuration(int32(ms));
            aura->SetDuration(int32(ms));
        }

        bool IsZombie(Unit* unit)
        {
            return unit->HasAura(SPELL_YOURE_A_ZOMBIE);
        }

        void Infect(Player* player)
        {
            if (!IsEventParticipant(player, EVENT_ZOMBIE_INFESTATION) || player->IsGameMaster() ||
                !player->IsAlive() || IsZombie(player) || player->HasAura(SPELL_YOURE_INFECTED))
                return;

            player->AddAura(SPELL_YOURE_INFECTED, player);
            TellPlayer(player, "|cff66cc33You're infected!|r Find an Argent Healer within a minute or you'll turn.");
        }

        void StripPlague(Player* player)
        {
            player->RemoveAurasDueToSpell(SPELL_YOURE_INFECTED);
            player->RemoveAurasDueToSpell(SPELL_YOURE_A_ZOMBIE);
            player->RemoveAurasDueToSpell(SPELL_ZOMBIE_FACTION);
        }

        class ZombieInfestationEvent : public EraEvent
        {
        public:
            ZombieInfestationEvent() : EraEvent(EVENT_ZOMBIE_INFESTATION) { }

            char const* GetKey() const override { return "zombie"; }
            char const* GetName() const override { return "Zombie Infestation"; }
            char const* GetConfigName() const override { return "ZombieInfestation"; }
            uint8 GetTier() const override { return 12; }

            bool Prepare(std::vector<Player*> const& cohort) override
            {
                uint32 alliance = 0;
                uint32 horde = 0;
                for (Player* player : cohort)
                    (player->GetTeamId() == TEAM_ALLIANCE ? alliance : horde)++;

                _city = alliance == horde ? urand(0, 1) : (alliance > horde ? 0 : 1);
                return true;
            }

            std::string GetWarningText(uint32 minutes) const override
            {
                return Acore::StringFormat("Crates of grain have been turning up in {} and people who ate from them are falling ill. "
                    "Something will happen in {} minutes...", CityRef().name, minutes);
            }

            std::string GetStartText() const override
            {
                return Acore::StringFormat("The plague has broken out in {}! The dead are rising around the grain crates in {}. "
                    "Smash the crates and put down the zombies. Argent Healers can cure the infected.", CityRef().name, CityRef().where);
            }

            std::string GetEndText(bool won) const override
            {
                return won
                    ? Acore::StringFormat("The last plagued crate in {} is destroyed and the dead lie still. The Scourge will not forget this.", CityRef().name)
                    : Acore::StringFormat("The Argent Crusade has quarantined {}. The plague's source was never found.", CityRef().name);
            }

            bool IsEventArea(uint32 mapId, uint32 zoneId, uint32 /*areaId*/) const override
            {
                return mapId == CityRef().mapId && zoneId == CityRef().zoneId;
            }

            bool Start() override
            {
                City const& city = CityRef();
                if (!SpawnAnchor(city.mapId, city.center, NPC_ZOMBIE_ANCHOR))
                    return false;

                _crates.clear();
                for (uint32 i = 0; i < CRATES; ++i)
                {
                    Position pos = RandomPointAround(city.center, 6.0f, 30.0f);
                    if (GameObject* crate = SummonObject(GO_PLAGUED_CRATE, pos))
                        _crates.emplace(crate->GetGUID(), pos);
                }

                for (uint32 i = 0; i < HEALERS; ++i)
                    Summon(NPC_ARGENT_HEALER, RandomPointAround(city.center, 2.0f, 6.0f));

                _raiseTimer = 5 * IN_MILLISECONDS;
                _checkTimer = CHECK_MS;
                return !_crates.empty();
            }

            void Update(uint32 diff) override
            {
                if (!GetAnchor())
                    return;

                if (_raiseTimer <= diff)
                {
                    _raiseTimer = RAISE_MS;
                    RaiseDead();
                }
                else
                    _raiseTimer -= diff;

                if (_checkTimer <= diff)
                {
                    _checkTimer = CHECK_MS;
                    CheckPlayers();

                    if (_crates.empty() && !CountAlive(NPC_PLAGUE_ZOMBIE) && !CountAlive(NPC_PLAGUED_RESIDENT))
                        MarkWon();
                }
                else
                    _checkTimer -= diff;
            }

            void OnObjectUsed(ObjectGuid guid, uint32 entry, Position const& pos) override
            {
                if (entry != GO_PLAGUED_CRATE || !_crates.erase(guid))
                    return;

                // What was inside comes out.
                for (uint8 i = 0; i < 2; ++i)
                    if (Creature* zombie = Summon(NPC_PLAGUE_ZOMBIE, RandomPointAround(pos, 1.0f, 3.0f)))
                        zombie->CastSpell(zombie, SPELL_MINION_SPAWN_IN, true);
            }

        protected:
            void OnStop() override
            {
                // Nobody stays infected or dead-but-walking once it's over.
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                    if (player && player->IsInWorld())
                        StripPlague(player);

                _crates.clear();
            }

        private:
            City const& CityRef() const { return CITIES[_city]; }

            void RaiseDead()
            {
                uint32 alive = CountAlive(NPC_PLAGUE_ZOMBIE) + CountAlive(NPC_PLAGUED_RESIDENT);
                Position const& center = CityRef().center;

                for (auto const& [guid, pos] : _crates)
                {
                    if (alive++ >= MAX_ZOMBIES)
                        break;

                    uint32 entry = urand(0, 3) ? NPC_PLAGUE_ZOMBIE : NPC_PLAGUED_RESIDENT;
                    if (Creature* zombie = Summon(entry, RandomPointAround(pos, 1.0f, 4.0f)))
                    {
                        zombie->CastSpell(zombie, SPELL_MINION_SPAWN_IN, true);
                        SendTo(zombie, RandomPointAround(center, 0.0f, 20.0f), false);
                    }
                }
            }

            // Infections running out, healers at work, and the zombies who died.
            void CheckPlayers()
            {
                std::list<Creature*> healers;
                if (Creature* anchor = GetAnchor())
                    anchor->GetCreatureListWithEntryInGrid(healers, NPC_ARGENT_HEALER, 60.0f);

                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                {
                    if (!player || !player->IsInWorld())
                        continue;

                    if (!IsEventParticipant(player, EVENT_ZOMBIE_INFESTATION) || !player->IsAlive())
                    {
                        StripPlague(player);
                        continue;
                    }

                    Aura* infection = player->GetAura(SPELL_YOURE_INFECTED);
                    if (!infection)
                        continue;

                    if (Creature* healer = NearestHealer(healers, player))
                    {
                        healer->CastSpell(player, SPELL_CURE_INFECTION, true);
                        player->RemoveAurasDueToSpell(SPELL_YOURE_INFECTED);
                        TellPlayer(player, "The Argent Healer burns the plague out of you.");
                        continue;
                    }

                    if (GameTime::GetGameTime().count() - infection->GetApplyTime() < INFECTION_MS / IN_MILLISECONDS)
                        continue;

                    player->RemoveAurasDueToSpell(SPELL_YOURE_INFECTED);
                    SetTimed(player->AddAura(SPELL_YOURE_A_ZOMBIE, player), ZOMBIE_MS);
                    SetTimed(player->AddAura(SPELL_ZOMBIE_FACTION, player), ZOMBIE_MS);
                    TellPlayer(player, "|cff66cc33You've turned!|r Use your groan to spread the plague to the living. "
                        "It wears off in three minutes, or when you die.");
                }
            }

            static Creature* NearestHealer(std::list<Creature*> const& healers, Player* player)
            {
                for (Creature* healer : healers)
                    if (healer->IsAlive() && healer->IsWithinDist(player, HEAL_RANGE))
                        return healer;
                return nullptr;
            }

            uint32 _city = 0;
            std::unordered_map<ObjectGuid, Position> _crates;
            uint32 _raiseTimer = 0;
            uint32 _checkTimer = 0;
        };
    }

    std::unique_ptr<EraEvent> CreateZombieInfestationEvent()
    {
        return std::make_unique<ZombieInfestationEvent>();
    }

    void ClearZombieInfection(Player* player)
    {
        if (player)
            StripPlague(player);
    }
}

using namespace EraEvents;

// Clicking a plagued crate smashes it; the event raises what was inside.
struct go_era_plagued_crate : public GameObjectAI
{
    go_era_plagued_crate(GameObject* object) : GameObjectAI(object) { }

    bool GossipHello(Player* player, bool /*reportUse*/) override
    {
        if (!IsEventParticipant(player, EVENT_ZOMBIE_INFESTATION))
            return true;

        if (me->isSpawned())
        {
            NotifyObjectUsed(me);
            me->DespawnOrUnsummon();
            TellPlayer(player, "You smash the crate. Something inside stirs...");
        }
        return true;
    }
};

// Beckoning Groan, on the zombie form's bar: infects the living players around the zombie.
class spell_era_beckoning_groan : public SpellScript
{
    PrepareSpellScript(spell_era_beckoning_groan);

    void HandleAfterCast()
    {
        Unit* caster = GetCaster();
        if (!caster || !caster->IsPlayer() || !IsZombie(caster) ||
            !IsEventParticipant(caster->ToPlayer(), EVENT_ZOMBIE_INFESTATION))
            return;

        std::list<Player*> players;
        Acore::AnyPlayerInObjectRangeCheck check(caster, GROAN_RANGE);
        Acore::PlayerListSearcher<Acore::AnyPlayerInObjectRangeCheck> searcher(caster, players, check);
        Cell::VisitObjects(caster, searcher, GROAN_RANGE);

        for (Player* player : players)
            if (player != caster)
                Infect(player);
    }

    void Register() override
    {
        AfterCast += SpellCastFn(spell_era_beckoning_groan::HandleAfterCast);
    }
};

class EraEventsZombiePlayerScript : public PlayerScript
{
public:
    EraEventsZombiePlayerScript() : PlayerScript("EraEventsZombiePlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_BEFORE_LOGOUT,
    }) { }

    void OnPlayerLogin(Player* player) override
    {
        ClearZombieInfection(player);
    }

    void OnPlayerBeforeLogout(Player* player) override
    {
        ClearZombieInfection(player);
    }
};

void AddEraEventZombieScripts()
{
    RegisterMob(NPC_PLAGUE_ZOMBIE, {
        .spells = {
            { SPELL_INFECTED_BITE, 12000, 18000, SpellTarget::Victim },
        },
        .hitAura = SPELL_YOURE_INFECTED,
        .hitAuraChance = 20,
        .hitAuraBlocker = SPELL_YOURE_A_ZOMBIE,
    });

    RegisterMob(NPC_PLAGUED_RESIDENT, {
        .hitAura = SPELL_YOURE_INFECTED,
        .hitAuraChance = 10,
        .hitAuraBlocker = SPELL_YOURE_A_ZOMBIE,
    });

    RegisterGameObjectAI(go_era_plagued_crate);
    RegisterSpellScript(spell_era_beckoning_groan);
    new EraEventsZombiePlayerScript();
}
