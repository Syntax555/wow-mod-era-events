/*
 * mod-era-events: shared declarations.
 *
 * Released under the MIT License.
 */

#ifndef MOD_ERA_EVENTS_H
#define MOD_ERA_EVENTS_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class Creature;
class GameObject;
class Map;
class Player;
class TempSummon;

namespace EraEvents
{
    constexpr uint8 IP_STATE_MAX = 18;

    enum EventType : uint8
    {
        EVENT_AQ_WAR,
        EVENT_SCOURGE_INVASION,
        EVENT_LEGION_INCURSION,
        EVENT_ZOMBIE_INFESTATION,
        MAX_ERA_EVENTS
    };

    // Each event owns a phase bit mod-individual-progression doesn't use (it uses bits 16-21) and a
    // server-side aura that adds that bit to a player. The auras are spell_dbc rows in data/sql.
    constexpr std::array<uint32, MAX_ERA_EVENTS> EVENT_PHASE_MASKS = { 1u << 22, 1u << 23, 1u << 24, 1u << 25 };
    constexpr std::array<uint32, MAX_ERA_EVENTS> EVENT_PHASE_SPELLS = { 90120, 90121, 90122, 90123 };

    enum Team : uint8
    {
        TEAM_ALLIANCE_ONLY,
        TEAM_HORDE_ONLY,
        TEAM_ANY,
    };

    // One run of a live event. The manager decides when it runs and who takes part; the event only
    // knows where it happens and what it spawns.
    class EraEvent
    {
    public:
        explicit EraEvent(EventType type) : _type(type) { }
        virtual ~EraEvent() = default;

        EventType GetType() const { return _type; }
        uint32 GetPhaseMask() const { return EVENT_PHASE_MASKS[_type]; }
        uint32 GetPhaseSpell() const { return EVENT_PHASE_SPELLS[_type]; }

        virtual char const* GetKey() const = 0;        // for commands: "si"
        virtual char const* GetName() const = 0;       // "Scourge Invasion"
        virtual char const* GetConfigName() const = 0; // conf prefix: "ScourgeInvasion"
        virtual uint8 GetTier() const = 0;             // IP state the event belongs to

        // Picks where this run happens. cohort is every real player at the event's tier who is
        // online. Returns false if the event can't run right now.
        virtual bool Prepare(std::vector<Player*> const& cohort) = 0;

        // Text for the cohort. Called after Prepare.
        virtual std::string GetWarningText(uint32 minutes) const = 0;
        virtual std::string GetStartText() const = 0;
        virtual std::string GetEndText(bool won) const = 0;

        // Whether a player standing here should be phased into the event.
        virtual bool IsEventArea(uint32 mapId, uint32 zoneId, uint32 areaId) const = 0;

        // A realm-wide run is open to every era: whoever stands in the event's area is phased
        // in whatever their tier, and everyone online is told. These are a second aura for such
        // a run (0 for none) and where it is given.
        virtual uint32 GetRealmWidePhaseSpell() const { return 0; }
        virtual bool IsRealmWidePhaseArea(uint32 /*mapId*/, uint32 /*zoneId*/, uint32 /*areaId*/) const { return false; }

        // Told to everyone phased in when the event pays out without ending (RewardNow).
        virtual std::string GetRewardText() const { return std::string(); }

        // Spawns the event. Returns false if nothing could be spawned.
        virtual bool Start() = 0;
        virtual void Update(uint32 /*diff*/) { }

        // One of the event's creatures died. Deaths are queued from the map threads and handed
        // over in the world update, so the creature may already be gone.
        virtual void OnSummonDied(ObjectGuid /*guid*/, uint32 /*entry*/) { }

        // A player used one of the event's gameobjects. Queued like deaths.
        virtual void OnObjectUsed(ObjectGuid /*guid*/, uint32 /*entry*/, Position const& /*pos*/) { }

        // Despawns everything the event spawned.
        void Stop();

        // Set by the manager before Prepare and Start.
        void SetRun(bool realmWide, std::string instigator);
        bool IsRealmWide() const { return _realmWide; }

        bool IsWon() const { return _won; }
        bool TakeRewardDue() { return std::exchange(_rewardDue, false); }
        bool IsSummon(ObjectGuid guid) const;
        bool IsObject(ObjectGuid guid) const;
        void Adopt(ObjectGuid guid) { _summons.push_back(guid); }

    protected:
        // Summons the invisible, always-updating anchor every other spawn is summoned from, so
        // they all inherit the event's phase. Loads the grids around the spot first.
        Creature* SpawnAnchor(uint32 mapId, Position const& pos, uint32 entry, float gridRadius = 150.0f);
        Creature* GetAnchor() const;
        Map* GetMap() const { return _map; }

        // Summons a creature in the event's phase. With a despawn time of 0 it stays until the
        // event stops.
        TempSummon* Summon(uint32 entry, Position const& pos, uint32 despawnMs = 0);

        // Places a gameobject in the event's phase until the event stops.
        GameObject* SummonObject(uint32 entry, Position const& pos);

        // Nearby point on the ground around center.
        Position RandomPointAround(Position const& center, float minDist, float maxDist) const;

        uint32 CountAlive(uint32 entry) const;
        void MarkWon() { _won = true; }

        // Pays everyone phased in now. The event goes on.
        void RewardNow() { _rewardDue = true; }

        // Who set a realm-wide run off. Empty when nobody did.
        std::string const& GetInstigator() const { return _instigator; }

        virtual void OnStop() { }

    private:
        EventType _type;
        Map* _map = nullptr;
        ObjectGuid _anchor;
        std::vector<ObjectGuid> _summons;
        std::vector<ObjectGuid> _objects;
        bool _won = false;
        bool _rewardDue = false;
        bool _realmWide = false;
        std::string _instigator;
    };

    std::unique_ptr<EraEvent> CreateAQWarEvent();
    std::unique_ptr<EraEvent> CreateScourgeInvasionEvent();
    std::unique_ptr<EraEvent> CreateLegionIncursionEvent();
    std::unique_ptr<EraEvent> CreateZombieInfestationEvent();

    // The realm-clamped IP state, 255 when the earned progression policy is unavailable.
    uint8 GetPlayerTier(Player* player);
    bool IsEventParticipant(Player* player, EventType type);
    void ClearZombieInfection(Player* player);

    // False for playerbots and GMs: they never count towards the players an event needs.
    bool IsRealPlayer(Player* player);

    // Called by the event creatures' AI. Safe from any map thread.
    void NotifyCreatureDied(Creature* creature);

    // An event creature summoned something of its own; the event despawns it with the rest.
    void NotifySummoned(Creature* summoner, Creature* summon);

    // A player used an event gameobject. Safe from any map thread.
    void NotifyObjectUsed(GameObject* object);

    // Starts an event now, skipping the interval and the player count. Safe from any map thread;
    // it happens on the next world update.
    void RequestStart(EventType type);

    // The same, as a realm-wide run set off by instigator: open to every era, for the event's
    // RealmWideMinutes. With that setting at 0 it is a plain RequestStart. A realm-wide run that
    // is already going starts its time over.
    void RequestRealmWideStart(EventType type, std::string instigator);
}

void AddEraEventMobScripts();
void AddEraEventScourgeScripts();
void AddEraEventAQScripts();
void AddEraEventLegionScripts();
void AddEraEventZombieScripts();

#endif
