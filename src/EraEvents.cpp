/*
 * mod-era-events
 *
 * Timed live world events for servers running mod-individual-progression. Every character there
 * lives in its own era; this module gives each era its iconic moments (the Scourge attacking the
 * capitals, the gates of Ahn'Qiraj, the Legion pouring through before the Dark Portal opens, the
 * plague before Wrath) as events that happen on a schedule.
 *
 * An event only exists for the characters at its tier: everything it spawns sits in a phase of its
 * own, and a character is phased in while standing in the event's zone. A scheduled event only
 * starts when enough real players at that tier are online (2 by default), so it's always something
 * you share. Playerbots at the tier are phased in too and fight alongside you.
 *
 * The exception is a realm-wide run, which is for every era at once: whoever stands in the zone is
 * phased in, and everyone online is told. The Scarab Gong starts one, the Ahn'Qiraj War.
 *
 * Commands:
 *   .era next                    what's coming up for your era
 *   .era list                    (GM) every event and its state
 *   .era start <event> [realm]   (GM) start an event now: aq, si, legion, zombie. With "realm" as
 *                                a realm-wide run, for the events that have one
 *   .era stop <event>            (GM) end a running event
 *
 * Released under the MIT License.
 */

#include "EraEvents.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Creature.h"
#include "DBCStores.h"
#include "DatabaseEnv.h"
#include "GameObject.h"
#include "Item.h"
#include "IndividualProgression.h"
#include "Log.h"
#include "Mail.h"
#include "Map.h"
#include "MapMgr.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QuestDef.h"
#include "ReputationMgr.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "TemporarySummon.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <mutex>
#include <type_traits>
#include <utility>

using namespace Acore::ChatCommands;

namespace EraEvents
{
    namespace
    {
        constexpr uint32 MINUTE_MS = MINUTE * IN_MILLISECONDS;
        constexpr uint32 PHASE_REFRESH_MS = 5 * IN_MILLISECONDS;
        constexpr uint32 CORPSE_DESPAWN_MS = 60 * IN_MILLISECONDS;

        struct EventSettings
        {
            bool enabled = true;
            uint32 intervalMin = 120;  // minutes
            uint32 intervalMax = 240;
            uint32 warning = 10;       // minutes of notice before it starts
            uint32 duration = 30;      // minutes before the event gives up
            uint32 realmWide = 0;      // minutes a realm-wide run lasts, 0 = the event has none
            uint32 rewardGold = 0;     // copper
            uint32 rewardFaction = 0;
            int32 rewardReputation = 0;
            uint32 rewardItem = 0;
            uint32 rewardItemCount = 0;
        };

        struct Config
        {
            bool enabled = true;
            uint32 minRealPlayers = 2;
            uint32 retry = 5;          // minutes between checks while waiting for players
            bool ipEnabled = false;    // IndividualProgression.Enable
            uint32 ipLimit = 0;        // IndividualProgression.ProgressionLimit, 0 = none
            std::array<EventSettings, MAX_ERA_EVENTS> events;
        };

        Config config;

        // Bots are sessions without a socket. AzerothCore marks them with WorldSession::IsHeadless();
        // older playerbots core forks have WorldSession::IsBot() instead, and older stock cores have
        // neither. Looking for both at compile time lets the module build on all of them.
        template <typename Session, typename = void>
        struct HasIsHeadless : std::false_type { };

        template <typename Session>
        struct HasIsHeadless<Session, std::void_t<decltype(std::declval<Session&>().IsHeadless())>> : std::true_type { };

        template <typename Session, typename = void>
        struct HasIsBot : std::false_type { };

        template <typename Session>
        struct HasIsBot<Session, std::void_t<decltype(std::declval<Session&>().IsBot())>> : std::true_type { };

        template <typename Session>
        bool IsBotSession(Session* session)
        {
            if constexpr (HasIsHeadless<Session>::value)
                return session->IsHeadless();
            else if constexpr (HasIsBot<Session>::value)
                return session->IsBot();
            else
                return false;
        }

        enum class State : uint8
        {
            Idle,     // waiting for the next attempt
            Warning,  // announced, spawns when the timer runs out
            Running,
        };

        char const* StateName(State state)
        {
            switch (state)
            {
                case State::Idle:    return "waiting";
                case State::Warning: return "announced";
                case State::Running: return "running";
            }
            return "?";
        }

        std::string FormatMinutes(uint32 ms)
        {
            uint32 minutes = (ms + MINUTE_MS - 1) / MINUTE_MS;
            return minutes == 1 ? "1 minute" : Acore::StringFormat("{} minutes", minutes);
        }

        void Tell(Player* player, std::string const& text, bool onScreen)
        {
            WorldSession* session = player->GetSession();
            if (!session || IsBotSession(session))
                return;

            ChatHandler handler(session);
            handler.SendSysMessage("|cffff8000[Era Event]|r " + text);
            if (onScreen)
                handler.SendNotification(text);
        }

        struct Slot
        {
            std::unique_ptr<EraEvent> event;
            State state = State::Idle;
            int64 timer = 0;
        };

        class Manager
        {
        public:
            static Manager& Instance()
            {
                static Manager instance;
                return instance;
            }

            void Init()
            {
                if (_initialized)
                    return;

                _initialized = true;
                _slots[EVENT_AQ_WAR].event = CreateAQWarEvent();
                _slots[EVENT_SCOURGE_INVASION].event = CreateScourgeInvasionEvent();
                _slots[EVENT_LEGION_INCURSION].event = CreateLegionIncursionEvent();
                _slots[EVENT_ZOMBIE_INFESTATION].event = CreateZombieInfestationEvent();

                for (Slot& slot : _slots)
                    if (slot.event)
                        slot.timer = RollInterval(slot.event->GetType());
            }

            EventSettings const& Settings(EventType type) const { return config.events[type]; }

            Slot* FindSlot(std::string_view key)
            {
                for (Slot& slot : _slots)
                    if (slot.event && key == slot.event->GetKey())
                        return &slot;
                return nullptr;
            }

            Slot* FindSlotForTier(uint8 tier)
            {
                for (Slot& slot : _slots)
                    if (slot.event && slot.event->GetTier() == tier)
                        return &slot;
                return nullptr;
            }

            std::array<Slot, MAX_ERA_EVENTS>& Slots() { return _slots; }

            bool IsAvailable(EraEvent const& event) const
            {
                if (!config.enabled || !config.ipEnabled || !Settings(event.GetType()).enabled ||
                    !sIndividualProgression->IsStrictDefaultProgression())
                    return false;

                // An era IP never lets anyone reach.
                return !config.ipLimit || event.GetTier() <= config.ipLimit;
            }

            std::vector<Player*> GetCohort(uint8 tier, bool realOnly) const
            {
                std::vector<Player*> cohort;
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                {
                    if (!player || !player->IsInWorld())
                        continue;

                    if (realOnly && !IsRealPlayer(player))
                        continue;

                    if (GetPlayerTier(player) == tier)
                        cohort.push_back(player);
                }
                return cohort;
            }

            // Who is told about a run: the event's era, or everyone when it is realm-wide.
            std::vector<Player*> GetAudience(EraEvent const& event) const
            {
                if (!event.IsRealmWide())
                    return GetCohort(event.GetTier(), false);

                std::vector<Player*> audience;
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                    if (player && player->IsInWorld())
                        audience.push_back(player);
                return audience;
            }

            void Update(uint32 diff)
            {
                if (!_initialized)
                    return;

                HandleQueued();

                bool anyRunning = false;
                for (Slot& slot : _slots)
                {
                    if (!slot.event)
                        continue;

                    UpdateSlot(slot, diff);
                    anyRunning |= slot.state == State::Running;
                }

                // Picks up tier changes and anyone the zone hooks missed. Also clears leftover
                // auras once the last event has ended.
                _phaseTimer += diff;
                if (_phaseTimer >= PHASE_REFRESH_MS && (anyRunning || _phasesDirty))
                {
                    _phaseTimer = 0;
                    _phasesDirty = anyRunning;
                    RefreshAllPhases();
                }
            }

            bool IsEventParticipant(Player* player, EventType type, uint32 zoneId, uint32 areaId) const
            {
                Slot const& slot = _slots[type];
                return player && player->IsInWorld() && slot.event && slot.state == State::Running &&
                    IsAvailable(*slot.event) && GetPlayerTier(player) == slot.event->GetTier() &&
                    slot.event->IsEventArea(player->GetMapId(), zoneId, areaId);
            }

            void UpdatePlayerPhases(Player* player, uint32 zoneId, uint32 areaId)
            {
                if (!player || !player->IsInWorld())
                    return;

                for (Slot const& slot : _slots)
                {
                    if (!slot.event)
                        continue;

                    EraEvent const& event = *slot.event;
                    bool want = IsEventParticipant(player, event.GetType(), zoneId, areaId);
                    SetAura(player, event.GetPhaseSpell(), want);

                    if (uint32 spell = event.GetRealmWidePhaseSpell())
                        SetAura(player, spell, want && event.IsRealmWide() &&
                            event.IsRealmWidePhaseArea(player->GetMapId(), zoneId, areaId));
                }

                if (!IsEventParticipant(player, EVENT_ZOMBIE_INFESTATION, zoneId, areaId))
                    ClearZombieInfection(player);
            }

            // Starts an event right away, skipping the notice and the player count.
            bool ForceStart(Slot& slot, std::string& error, bool realmWide = false, std::string instigator = {})
            {
                if (slot.state == State::Running)
                {
                    error = "it's already running";
                    return false;
                }

                if (realmWide && !Settings(slot.event->GetType()).realmWide)
                {
                    error = "it has no realm-wide run (RealmWideMinutes is 0)";
                    return false;
                }

                slot.event->SetRun(realmWide, std::move(instigator));

                std::vector<Player*> cohort = GetCohort(slot.event->GetTier(), true);
                if (slot.state == State::Idle && !slot.event->Prepare(cohort))
                {
                    slot.event->SetRun(false, {});
                    error = "it couldn't pick a place to happen";
                    return false;
                }

                if (!Begin(slot))
                {
                    slot.event->SetRun(false, {});
                    error = "nothing could be spawned, see the server log";
                    return false;
                }
                return true;
            }

            void ForceStop(Slot& slot)
            {
                if (slot.state == State::Idle)
                    return;
                Finish(slot, false);
            }

            void QueueDeath(ObjectGuid guid, uint32 entry)
            {
                std::lock_guard<std::mutex> lock(_queueLock);
                _deaths.emplace_back(guid, entry);
            }

            void QueueUse(ObjectGuid guid, uint32 entry, Position const& pos)
            {
                std::lock_guard<std::mutex> lock(_queueLock);
                _uses.push_back({ guid, entry, pos });
            }

            void QueueAdopt(ObjectGuid summoner, ObjectGuid summon)
            {
                std::lock_guard<std::mutex> lock(_queueLock);
                _adoptions.emplace_back(summoner, summon);
            }

            void QueueStart(EventType type, bool realmWide = false, std::string instigator = {})
            {
                std::lock_guard<std::mutex> lock(_queueLock);
                _starts.push_back({ type, realmWide, std::move(instigator) });
            }

        private:
            int64 RollInterval(EventType type) const
            {
                EventSettings const& s = Settings(type);
                uint32 low = std::max<uint32>(1, s.intervalMin);
                uint32 high = std::max(low, s.intervalMax);
                return int64(urand(low, high)) * MINUTE_MS;
            }

            void HandleQueued()
            {
                std::vector<std::pair<ObjectGuid, uint32>> deaths;
                std::vector<std::pair<ObjectGuid, ObjectGuid>> adoptions;
                std::vector<ObjectUse> uses;
                std::vector<StartRequest> starts;
                {
                    std::lock_guard<std::mutex> lock(_queueLock);
                    deaths.swap(_deaths);
                    adoptions.swap(_adoptions);
                    uses.swap(_uses);
                    starts.swap(_starts);
                }

                for (ObjectUse const& use : uses)
                    for (Slot& slot : _slots)
                        if (slot.event && slot.state == State::Running && slot.event->IsObject(use.guid))
                            slot.event->OnObjectUsed(use.guid, use.entry, use.pos);

                for (auto const& [summoner, summon] : adoptions)
                    for (Slot& slot : _slots)
                        if (slot.event && slot.state == State::Running && slot.event->IsSummon(summoner))
                            slot.event->Adopt(summon);

                for (auto const& [guid, entry] : deaths)
                    for (Slot& slot : _slots)
                        if (slot.event && slot.state == State::Running && slot.event->IsSummon(guid))
                            slot.event->OnSummonDied(guid, entry);

                for (StartRequest& request : starts)
                {
                    Slot& slot = _slots[request.type];
                    if (!slot.event || !IsAvailable(*slot.event))
                        continue;

                    EraEvent& event = *slot.event;
                    bool realmWide = request.realmWide && Settings(request.type).realmWide;

                    if (slot.state == State::Running)
                    {
                        if (!realmWide)
                            continue;

                        if (event.IsRealmWide())
                        {
                            // Set off again while it runs: its time starts over.
                            event.SetRun(true, std::move(request.instigator));
                            slot.timer = RunTime(event);
                            for (Player* player : GetAudience(event))
                                Tell(player, StartText(event, slot.timer), true);
                            continue;
                        }

                        // A run for the event's own era is going. The realm-wide one takes its place.
                        event.Stop();
                        slot.state = State::Idle;
                    }

                    std::string error;
                    if (!ForceStart(slot, error, realmWide, std::move(request.instigator)))
                        LOG_WARN("module", "mod-era-events: couldn't start {}: {}", event.GetName(), error);
                }
            }

            static void SetAura(Player* player, uint32 spell, bool want)
            {
                bool has = player->HasAura(spell);
                if (want && !has)
                    player->CastSpell(player, spell, true);
                else if (!want && has)
                    player->RemoveAurasDueToSpell(spell);
            }

            int64 RunTime(EraEvent const& event) const
            {
                EventSettings const& s = Settings(event.GetType());
                return int64(std::max<uint32>(1, event.IsRealmWide() ? s.realmWide : s.duration)) * MINUTE_MS;
            }

            // A realm-wide run is announced to players who know nothing about it, so it says how long.
            static std::string StartText(EraEvent const& event, int64 timer)
            {
                std::string text = event.GetStartText();
                if (event.IsRealmWide())
                    text += Acore::StringFormat(" It lasts {}.", FormatMinutes(uint32(std::max<int64>(0, timer))));
                return text;
            }

            void UpdateSlot(Slot& slot, uint32 diff)
            {
                EraEvent& event = *slot.event;

                if (!IsAvailable(event))
                {
                    if (slot.state != State::Idle)
                        ForceStop(slot);
                    return;
                }

                if (slot.state == State::Running)
                {
                    event.Update(diff);
                    if (event.TakeRewardDue())
                        RewardParticipants(event, event.GetRewardText());

                    if (event.IsWon())
                    {
                        Finish(slot, true);
                        return;
                    }
                }

                slot.timer -= diff;
                if (slot.timer > 0)
                    return;

                switch (slot.state)
                {
                    case State::Idle:
                        TryAnnounce(slot);
                        break;
                    case State::Warning:
                        if (!Begin(slot))
                            Reschedule(slot, int64(config.retry) * MINUTE_MS);
                        break;
                    case State::Running:
                        Finish(slot, false);
                        break;
                }
            }

            void TryAnnounce(Slot& slot)
            {
                EraEvent& event = *slot.event;
                int64 retry = int64(std::max<uint32>(1, config.retry)) * MINUTE_MS;

                if (!IsAvailable(event))
                {
                    slot.timer = retry;
                    return;
                }

                std::vector<Player*> cohort = GetCohort(event.GetTier(), true);
                if (cohort.size() < config.minRealPlayers)
                {
                    slot.timer = retry;
                    return;
                }

                event.SetRun(false, {});
                if (!event.Prepare(cohort))
                {
                    slot.timer = retry;
                    return;
                }

                uint32 warning = Settings(event.GetType()).warning;
                if (!warning)
                {
                    if (!Begin(slot))
                        Reschedule(slot, retry);
                    return;
                }

                slot.state = State::Warning;
                slot.timer = int64(warning) * MINUTE_MS;
                for (Player* player : GetCohort(event.GetTier(), false))
                    Tell(player, event.GetWarningText(warning), true);

                LOG_INFO("module", "mod-era-events: {} announced, starting in {} minutes", event.GetName(), warning);
            }

            bool Begin(Slot& slot)
            {
                EraEvent& event = *slot.event;
                if (!IsAvailable(event))
                    return false;

                if (!event.Start())
                {
                    LOG_ERROR("module", "mod-era-events: {} failed to spawn", event.GetName());
                    event.Stop();
                    return false;
                }

                slot.state = State::Running;
                slot.timer = RunTime(event);
                for (Player* player : GetAudience(event))
                    Tell(player, StartText(event, slot.timer), true);

                _phasesDirty = true;
                RefreshAllPhases();
                LOG_INFO("module", "mod-era-events: {} started{}", event.GetName(), event.IsRealmWide() ? ", realm-wide" : "");
                return true;
            }

            void Finish(Slot& slot, bool won)
            {
                EraEvent& event = *slot.event;
                if (slot.state == State::Running)
                {
                    if (won)
                        RewardParticipants(event);

                    for (Player* player : GetAudience(event))
                        Tell(player, event.GetEndText(won), true);
                }

                event.Stop();
                event.SetRun(false, {});
                Reschedule(slot, RollInterval(event.GetType()));
                RefreshAllPhases();
                LOG_INFO("module", "mod-era-events: {} ended ({})", event.GetName(), won ? "won" : "lost or stopped");
            }

            void Reschedule(Slot& slot, int64 timer)
            {
                slot.state = State::Idle;
                slot.timer = timer;
            }

            void RefreshAllPhases()
            {
                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                    if (player && player->IsInWorld())
                        UpdatePlayerPhases(player, player->GetZoneId(), player->GetAreaId());
            }

            // text: also told to each of them, when there is any.
            void RewardParticipants(EraEvent const& event, std::string const& text = {})
            {
                EventSettings const& s = Settings(event.GetType());
                FactionEntry const* faction = s.rewardFaction ? sFactionStore.LookupEntry(s.rewardFaction) : nullptr;

                for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
                {
                    if (!player || !player->IsInWorld() || !player->HasAura(event.GetPhaseSpell()))
                        continue;

                    if (s.rewardGold)
                        player->ModifyMoney(s.rewardGold);

                    if (faction && s.rewardReputation)
                        player->GetReputationMgr().ModifyReputation(faction, float(s.rewardReputation));

                    if (s.rewardItem && s.rewardItemCount)
                        GiveItem(player, event, s.rewardItem, s.rewardItemCount);

                    if (!text.empty())
                        Tell(player, text, true);
                }
            }

            void GiveItem(Player* player, EraEvent const& event, uint32 itemId, uint32 count)
            {
                ItemPosCountVec dest;
                if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, itemId, count) == EQUIP_ERR_OK)
                {
                    if (Item* item = player->StoreNewItem(dest, itemId, true))
                        player->SendNewItem(item, count, true, false);
                    return;
                }

                // Bags full: it comes by mail instead.
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                MailDraft draft(event.GetName(), "Your share of the spoils. Your bags were too full to carry it.");
                if (Item* item = Item::CreateItem(itemId, count, player))
                {
                    item->SaveToDB(trans);
                    draft.AddItem(item);
                }
                draft.SendMailTo(trans, MailReceiver(player), MailSender(MAIL_NORMAL, 0, MAIL_STATIONERY_GM));
                CharacterDatabase.CommitTransaction(trans);
            }

            bool _initialized = false;
            std::array<Slot, MAX_ERA_EVENTS> _slots;
            uint32 _phaseTimer = 0;
            bool _phasesDirty = true; // clear auras saved from before a restart

            std::mutex _queueLock;
            std::vector<std::pair<ObjectGuid, uint32>> _deaths;
            std::vector<std::pair<ObjectGuid, ObjectGuid>> _adoptions;

            struct ObjectUse
            {
                ObjectGuid guid;
                uint32 entry;
                Position pos;
            };
            std::vector<ObjectUse> _uses;

            struct StartRequest
            {
                EventType type;
                bool realmWide;
                std::string instigator;
            };
            std::vector<StartRequest> _starts;
        };

        Manager& Mgr() { return Manager::Instance(); }

        void LoadConfig()
        {
            config.enabled = sConfigMgr->GetOption<bool>("EraEvents.Enable", true);
            config.minRealPlayers = sConfigMgr->GetOption<uint32>("EraEvents.MinRealPlayers", 2);
            config.retry = std::max<uint32>(1, sConfigMgr->GetOption<uint32>("EraEvents.RetryMinutes", 5));

            // mod-individual-progression's own settings; the third argument keeps the core quiet
            // when IP isn't installed.
            config.ipEnabled = sConfigMgr->GetOption<bool>("IndividualProgression.Enable", false, false);
            config.ipLimit = sConfigMgr->GetOption<uint32>("IndividualProgression.ProgressionLimit", 0, false);

            // Per-event defaults, then the conf file.
            struct Defaults
            {
                char const* name;
                uint32 duration;
                uint32 realmWide;
                uint32 gold;
                uint32 faction;
                int32 reputation;
                uint32 item;
                uint32 itemCount;
            };

            std::array<Defaults, MAX_ERA_EVENTS> const defaults = {{
                { "AQWar",             45, 120, 20 * GOLD, 609, 250, 0,     0 }, // Cenarion Circle
                { "ScourgeInvasion",   30, 0,   15 * GOLD, 529, 250, 22484, 5 }, // Argent Dawn, Necrotic Runes
                { "LegionIncursion",   30, 0,   20 * GOLD, 529, 150, 0,     0 }, // Argent Dawn
                { "ZombieInfestation", 30, 0,   50 * GOLD, 529, 250, 0,     0 }, // Argent Dawn
            }};

            for (uint8 i = 0; i < MAX_ERA_EVENTS; ++i)
            {
                Defaults const& d = defaults[i];
                std::string prefix = Acore::StringFormat("EraEvents.{}.", d.name);
                EventSettings& s = config.events[i];

                s.enabled = sConfigMgr->GetOption<bool>(prefix + "Enable", true);
                s.intervalMin = sConfigMgr->GetOption<uint32>(prefix + "IntervalMin", 120);
                s.intervalMax = sConfigMgr->GetOption<uint32>(prefix + "IntervalMax", 240);
                s.warning = sConfigMgr->GetOption<uint32>(prefix + "WarningMinutes", 10);
                s.duration = sConfigMgr->GetOption<uint32>(prefix + "DurationMinutes", d.duration);
                // Only the events with a realm-wide run have the key in the conf file.
                s.realmWide = sConfigMgr->GetOption<uint32>(prefix + "RealmWideMinutes", d.realmWide, d.realmWide != 0);
                s.rewardGold = sConfigMgr->GetOption<uint32>(prefix + "RewardGold", d.gold / GOLD) * GOLD;
                s.rewardFaction = sConfigMgr->GetOption<uint32>(prefix + "RewardFaction", d.faction);
                s.rewardReputation = sConfigMgr->GetOption<int32>(prefix + "RewardReputation", d.reputation);
                s.rewardItem = sConfigMgr->GetOption<uint32>(prefix + "RewardItem", d.item);
                s.rewardItemCount = sConfigMgr->GetOption<uint32>(prefix + "RewardItemCount", d.itemCount);

                // Events require the strict default chain. Clamp independently of IP's
                // config hook order: zone presence earns neither access nor rewards.
                s.realmWide = 0;
                s.rewardGold = 0;
                s.rewardFaction = 0;
                s.rewardReputation = 0;
                s.rewardItem = 0;
                s.rewardItemCount = 0;
            }

            if (config.enabled && !config.ipEnabled)
                LOG_WARN("module", "mod-era-events: IndividualProgression.Enable is off, so no era has players and no event will run.");
        }
    }

    // --- Shared helpers -------------------------------------------------------------------------

    uint8 GetPlayerTier(Player* player)
    {
        if (!config.ipEnabled || !player || !player->IsInWorld() ||
            !sIndividualProgression->IsStrictDefaultProgression())
            return 255;

        uint8 state = sIndividualProgression->GetPlayerProgressionFromQuests(player);
        if (state > IP_STATE_MAX)
            return 255;

        if (sIndividualProgression->progressionLimit > 0)
            state = uint8(std::min<int>(state, sIndividualProgression->progressionLimit));
        return state;
    }

    bool IsEventParticipant(Player* player, EventType type)
    {
        return player && Mgr().IsEventParticipant(player, type, player->GetZoneId(), player->GetAreaId());
    }

    bool IsRealPlayer(Player* player)
    {
        WorldSession* session = player ? player->GetSession() : nullptr;
        return session && !IsBotSession(session) && !player->IsGameMaster();
    }

    void NotifyCreatureDied(Creature* creature)
    {
        if (creature)
            Mgr().QueueDeath(creature->GetGUID(), creature->GetEntry());
    }

    void NotifySummoned(Creature* summoner, Creature* summon)
    {
        if (summoner && summon)
            Mgr().QueueAdopt(summoner->GetGUID(), summon->GetGUID());
    }

    void NotifyObjectUsed(GameObject* object)
    {
        if (object)
            Mgr().QueueUse(object->GetGUID(), object->GetEntry(), object->GetPosition());
    }

    void RequestStart(EventType type)
    {
        Mgr().QueueStart(type);
    }

    void RequestRealmWideStart(EventType type, std::string instigator)
    {
        Mgr().QueueStart(type, true, std::move(instigator));
    }

    // --- EraEvent -------------------------------------------------------------------------------

    Creature* EraEvent::SpawnAnchor(uint32 mapId, Position const& pos, uint32 entry, float gridRadius)
    {
        Map* map = sMapMgr->CreateBaseMap(mapId);
        if (!map || map->Instanceable())
            return nullptr;

        // Load the spot so the spawns land among the world's own creatures and stay updated with
        // nobody around.
        map->LoadGridsInRange(pos, gridRadius);

        TempSummon* anchor = map->SummonCreature(entry, pos);
        if (!anchor)
            return nullptr;

        anchor->SetPhaseMask(GetPhaseMask(), true);
        anchor->setActive(true);

        _map = map;
        _anchor = anchor->GetGUID();
        _won = false;
        return anchor;
    }

    Creature* EraEvent::GetAnchor() const
    {
        return _map ? _map->GetCreature(_anchor) : nullptr;
    }

    TempSummon* EraEvent::Summon(uint32 entry, Position const& pos, uint32 despawnMs)
    {
        Creature* anchor = GetAnchor();
        if (!anchor)
            return nullptr;

        // Summoning from the anchor puts the creature in the event's phase.
        TempSummon* summon = despawnMs
            ? anchor->SummonCreature(entry, pos, TEMPSUMMON_TIMED_DESPAWN, despawnMs)
            : anchor->SummonCreature(entry, pos, TEMPSUMMON_CORPSE_TIMED_DESPAWN, CORPSE_DESPAWN_MS);

        if (summon)
            _summons.push_back(summon->GetGUID());
        return summon;
    }

    GameObject* EraEvent::SummonObject(uint32 entry, Position const& pos)
    {
        Creature* anchor = GetAnchor();
        if (!anchor)
            return nullptr;

        // Summoned from the anchor, so it's in the event's phase. Lasts a day; Stop removes it.
        GameObject* object = anchor->SummonGameObject(entry, pos.GetPositionX(), pos.GetPositionY(), pos.GetPositionZ(),
            pos.GetOrientation(), 0.0f, 0.0f, 0.0f, 0.0f, DAY);
        if (object)
            _objects.push_back(object->GetGUID());
        return object;
    }

    Position EraEvent::RandomPointAround(Position const& center, float minDist, float maxDist) const
    {
        Creature* anchor = GetAnchor();
        float angle = frand(0.0f, 2.0f * float(M_PI));
        float dist = frand(minDist, maxDist);
        float x = center.GetPositionX() + dist * std::cos(angle);
        float y = center.GetPositionY() + dist * std::sin(angle);
        float z = center.GetPositionZ();

        if (anchor)
        {
            float ground = anchor->GetMapHeight(x, y, z + 5.0f);
            if (ground > INVALID_HEIGHT && std::fabs(ground - z) < 15.0f)
                z = ground;
            else
            {
                // Off a cliff or into a wall: stay on the center.
                x = center.GetPositionX();
                y = center.GetPositionY();
            }
        }

        return Position(x, y, z, angle);
    }

    uint32 EraEvent::CountAlive(uint32 entry) const
    {
        if (!_map)
            return 0;

        uint32 count = 0;
        for (ObjectGuid const& guid : _summons)
            if (guid.GetEntry() == entry)
                if (Creature* creature = _map->GetCreature(guid))
                    if (creature->IsAlive())
                        ++count;
        return count;
    }

    bool EraEvent::IsSummon(ObjectGuid guid) const
    {
        return std::find(_summons.begin(), _summons.end(), guid) != _summons.end();
    }

    bool EraEvent::IsObject(ObjectGuid guid) const
    {
        return std::find(_objects.begin(), _objects.end(), guid) != _objects.end();
    }

    void EraEvent::Stop()
    {
        OnStop();

        if (_map)
        {
            for (ObjectGuid const& guid : _summons)
                if (Creature* creature = _map->GetCreature(guid))
                    creature->DespawnOrUnsummon();

            for (ObjectGuid const& guid : _objects)
                if (GameObject* object = _map->GetGameObject(guid))
                    object->DespawnOrUnsummon();

            if (Creature* anchor = _map->GetCreature(_anchor))
                anchor->DespawnOrUnsummon();
        }

        _summons.clear();
        _objects.clear();
        _anchor.Clear();
        _map = nullptr;
        _rewardDue = false;
    }

    void EraEvent::SetRun(bool realmWide, std::string instigator)
    {
        _realmWide = realmWide;
        _instigator = std::move(instigator);
    }
}

using namespace EraEvents;

class EraEventsWorldScript : public WorldScript
{
public:
    EraEventsWorldScript() : WorldScript("EraEventsWorldScript", {
        WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_STARTUP,
        WORLDHOOK_ON_UPDATE,
    }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }

    void OnStartup() override
    {
        Mgr().Init();
    }

    void OnUpdate(uint32 diff) override
    {
        Mgr().Update(diff);
    }
};

class EraEventsPlayerScript : public PlayerScript
{
public:
    EraEventsPlayerScript() : PlayerScript("EraEventsPlayerScript", {
        PLAYERHOOK_ON_LOGIN,
        PLAYERHOOK_ON_UPDATE_ZONE,
    }) { }

    // Also drops an aura saved at logout from an event that has ended since.
    void OnPlayerLogin(Player* player) override
    {
        Mgr().UpdatePlayerPhases(player, player->GetZoneId(), player->GetAreaId());
    }

    void OnPlayerUpdateZone(Player* player, uint32 newZone, uint32 newArea) override
    {
        Mgr().UpdatePlayerPhases(player, newZone, newArea);
    }
};

class EraEventsCommandScript : public CommandScript
{
public:
    EraEventsCommandScript() : CommandScript("EraEventsCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable eraTable =
        {
            { "next",  HandleNext,  SEC_PLAYER,     Console::No },
            { "list",  HandleList,  SEC_GAMEMASTER, Console::Yes },
            { "start", HandleStart, SEC_GAMEMASTER, Console::Yes },
            { "stop",  HandleStop,  SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "era", eraTable },
        };

        return commandTable;
    }

    static bool HandleNext(ChatHandler* handler)
    {
        Player* player = handler->GetPlayer();
        if (!player || !config.enabled)
            return false;

        // A realm-wide run is everyone's, whatever their era.
        for (Slot& running : Mgr().Slots())
        {
            if (!running.event || running.state != State::Running || !running.event->IsRealmWide())
                continue;

            handler->PSendSysMessage("{} is happening now, for every era, for another {}. {}", running.event->GetName(),
                FormatMinutes(uint32(std::max<int64>(0, running.timer))), running.event->GetStartText());
            return true;
        }

        uint8 tier = GetPlayerTier(player);
        Slot* slot = Mgr().FindSlotForTier(tier);
        if (!slot || !Mgr().IsAvailable(*slot->event))
        {
            handler->PSendSysMessage("No live event belongs to your era (tier {}).", tier);
            return true;
        }

        EraEvent const& event = *slot->event;
        uint32 timer = uint32(std::max<int64>(0, slot->timer));
        switch (slot->state)
        {
            case State::Running:
                handler->PSendSysMessage("{} is happening now. {}", event.GetName(), event.GetStartText());
                break;
            case State::Warning:
                handler->PSendSysMessage("{} starts in {}. {}", event.GetName(), FormatMinutes(timer),
                    event.GetWarningText(timer / MINUTE_MS));
                break;
            case State::Idle:
            {
                std::size_t online = Mgr().GetCohort(tier, true).size();
                handler->PSendSysMessage("Your era's event is the {}. The next chance comes in about {}.",
                    event.GetName(), FormatMinutes(timer));
                handler->PSendSysMessage("It needs {} players of your era online; {} are online now.",
                    config.minRealPlayers, online);
                break;
            }
        }
        return true;
    }

    static bool HandleList(ChatHandler* handler)
    {
        for (Slot& slot : Mgr().Slots())
        {
            if (!slot.event)
                continue;

            EraEvent const& event = *slot.event;
            handler->PSendSysMessage("{} ({}): tier {}, {}{}{}, next step in {}, {} real players at the tier online",
                event.GetName(), event.GetKey(), event.GetTier(), StateName(slot.state),
                slot.state == State::Running && event.IsRealmWide() ? " realm-wide" : "",
                Mgr().IsAvailable(event) ? "" : " (disabled)", FormatMinutes(uint32(std::max<int64>(0, slot.timer))),
                Mgr().GetCohort(event.GetTier(), true).size());
        }
        return true;
    }

    static Slot* FindOrComplain(ChatHandler* handler, std::string const& key)
    {
        Slot* slot = Mgr().FindSlot(key);
        if (!slot)
        {
            handler->SendSysMessage("No such event. Use .era list to see them.");
            handler->SetSentErrorMessage(true);
        }
        return slot;
    }

    static bool HandleStart(ChatHandler* handler, std::string key, Optional<std::string> mode)
    {
        Slot* slot = FindOrComplain(handler, key);
        if (!slot)
            return false;

        bool realmWide = mode && *mode == "realm";
        if (mode && !realmWide)
        {
            handler->SendSysMessage("The only thing that can follow the event is \"realm\", for a realm-wide run.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (!Mgr().IsAvailable(*slot->event))
        {
            handler->PSendSysMessage("{} is switched off (EraEvents.Enable, EraEvents.{}.Enable, "
                "IndividualProgression.Enable or IndividualProgression.ProgressionLimit).",
                slot->event->GetName(), slot->event->GetConfigName());
            handler->SetSentErrorMessage(true);
            return false;
        }

        std::string error;
        Player* gm = handler->GetPlayer();
        if (!Mgr().ForceStart(*slot, error, realmWide, gm ? gm->GetName() : "A Game Master"))
        {
            handler->PSendSysMessage("{} didn't start: {}.", slot->event->GetName(), error);
            handler->SetSentErrorMessage(true);
            return false;
        }

        handler->PSendSysMessage("{} started. {}", slot->event->GetName(), slot->event->GetStartText());
        return true;
    }

    static bool HandleStop(ChatHandler* handler, std::string key)
    {
        Slot* slot = FindOrComplain(handler, key);
        if (!slot)
            return false;

        Mgr().ForceStop(*slot);
        handler->PSendSysMessage("{} stopped.", slot->event->GetName());
        return true;
    }
};

void AddEraEventsScripts()
{
    new EraEventsWorldScript();
    new EraEventsPlayerScript();
    new EraEventsCommandScript();
    AddEraEventMobScripts();
    AddEraEventAQScripts();
    AddEraEventScourgeScripts();
    AddEraEventLegionScripts();
    AddEraEventZombieScripts();
}
