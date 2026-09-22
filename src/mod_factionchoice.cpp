/*
 * mod-factionchoice - play any race on either faction
 *
 * A character picks its side with ".faction alliance" / ".faction horde".
 * The choice is stored per character (through the core's player settings, so
 * "EnablePlayerSettings" must be on) and applied on every login.
 *
 * Applying means two things:
 *
 *   team    - Player::setTeamId(), which is what the server uses for
 *             battleground sides, graveyards, auction houses, chat channels,
 *             grouping and so on.
 *   faction - the unit faction template, which is what decides who is hostile,
 *             so a Horde Human is attacked by Stormwind guards and welcomed in
 *             Orgrimmar.
 *
 * The faction template alone is not enough. When a *creature* works out how it
 * feels about a *player*, the core takes the CvP branch of
 * Unit::GetFactionReactionTo() and uses the player's reputation with the
 * creature's faction instead of the faction templates. Reputation starting
 * values come from the character's race, so an Orc playing Alliance is still
 * at war with Stormwind: its NPCs come out unfriendly, which shows up as red
 * name plates and, because Player::GetNPCIfCanInteractWith() refuses anything
 * at or below unfriendly, as being unable to talk to anyone.
 *
 * So the reputations are rebased as well: every reputation is moved from the
 * starting values of the character's own race onto the starting values a
 * character of the chosen side would have, keeping whatever the character
 * earned on top. Which baseline is currently applied is remembered, so this is
 * a transition rather than something that drifts every time it runs.
 *
 * The core resets both to the character's race defaults in
 * Player::SetFactionForRace(), which happens on login and whenever a
 * mind control or disguise effect ends. That function calls the
 * OnPlayerUpdateFaction hook, so the override is re-applied immediately; the
 * per-update check below is the safety net for anything the hook misses.
 *
 * Deliberately not touched: effects that are *supposed* to change your faction
 * (mind control, faction-modifying auras) are left alone while they last.
 */

#include "Chat.h"
#include "Config.h"
#include "DBCStores.h"
#include "DataMap.h"
#include "Log.h"
#include "Map.h"
#include "MapMgr.h"
#include "DBCStores.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerSettings.h"
#include "DBCEnums.h"
#include "RaceMgr.h"
#include "ReputationMgr.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "StringFormat.h"
#include "SharedDefines.h"
#include "SpellAuraDefines.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace Acore::ChatCommands;

namespace
{
    constexpr char const* SETTINGS_SOURCE = "mod-factionchoice";
    constexpr uint32 SETTING_TEAM         = 0;   // 0 = race default, 1 = Alliance, 2 = Horde
    constexpr uint32 SETTING_REP_BASELINE = 1;   // which side's reputation baseline is applied
    constexpr uint32 SETTING_PROMPTED     = 2;   // 0 = never asked, 1 = answered the login prompt

    // Menus are sent with the player's own GUID, which the core supports
    // (WorldSession::HandleGossipSelectOptionOpcode has a branch for player
    // GUIDs) and answers through the OnPlayerGossipSelect hook.
    constexpr uint32 MENU_FACTION      = 90010;
    constexpr uint32 MENU_DESTINATION  = 90011;
    constexpr uint32 TEXT_FACTION      = 90010;   // npc_text rows, see sql/04_factionchoice_gossip.sql
    constexpr uint32 TEXT_DESTINATION  = 90011;

    constexpr uint32 SENDER_FACTION     = 0xFC01;
    constexpr uint32 SENDER_DESTINATION = 0xFC02;

    constexpr uint32 ACTION_ALLIANCE = 1;
    constexpr uint32 ACTION_HORDE    = 2;
    constexpr uint32 ACTION_KEEP     = 3;
    constexpr uint32 ACTION_CAPITAL  = 0xFF;      // in the destination menu; anything else is a race

    // Same encoding as SETTING_TEAM: 0 = the character's own race.
    constexpr uint32 BASELINE_OWN_RACE = 0;

    // The reputation state flags that describe a *side* rather than a player's
    // own choices, and so are the ones adopted when a character changes sides.
    // FACTION_FLAG_INACTIVE is deliberately absent: that one is the player's,
    // set from the reputation pane.
    constexpr uint32 ADOPTED_FLAGS = FACTION_FLAG_VISIBLE | FACTION_FLAG_AT_WAR |
                                     FACTION_FLAG_HIDDEN | FACTION_FLAG_INVISIBLE_FORCED |
                                     FACTION_FLAG_PEACE_FORCED | FACTION_FLAG_RIVAL |
                                     FACTION_FLAG_SPECIAL;
    constexpr char const* DATA_KEY        = "FactionChoiceData";

    // A race whose faction template stands in for the chosen side.
    constexpr uint8 PROXY_RACE[2] = { RACE_HUMAN, RACE_ORC };   // indexed by TeamId

    struct FactionChoiceConfig
    {
        bool        Enable          = true;
        bool        TeleportOnSwitch = true;
        bool        SetHomebind      = true;
        bool        AnnounceOnLogin  = true;
        bool        AlignReputations = true;
        uint8       StarterZoneMaxLevel = 4;
        bool        PromptOnFirstLogin = true;
        uint32      PromptDelay = 3000;
        uint8       PromptRetries = 4;
        uint32      PromptRetrySeconds = 20;
        std::string AllianceCapital  = "Stormwind";
        std::string HordeCapital     = "Orgrimmar";
    };

    FactionChoiceConfig cfg;

    // Cached per character so the per-update check is a single lookup and does
    // not have to touch the settings container.
    struct FactionChoiceData : public DataMap::Base
    {
        TeamId team = TEAM_NEUTRAL;
        uint32 promptTimer = 0;     // counts down after login, then the menu is sent
        uint8  promptTries = 0;     // the client can miss the first attempt
        TeamId choosingTeam = TEAM_NEUTRAL;   // side picked, destination still to come
        TeamId pendingTeam = TEAM_NEUTRAL;    // side to take up once the journey ends
        uint32 applyTimer = 0;
    };

    void LoadConfig()
    {
        cfg.Enable           = sConfigMgr->GetOption<bool>("FactionChoice.Enable", true);
        cfg.TeleportOnSwitch = sConfigMgr->GetOption<bool>("FactionChoice.TeleportOnSwitch", true);
        cfg.SetHomebind      = sConfigMgr->GetOption<bool>("FactionChoice.SetHomebind", true);
        cfg.AnnounceOnLogin  = sConfigMgr->GetOption<bool>("FactionChoice.AnnounceOnLogin", true);
        cfg.AlignReputations = sConfigMgr->GetOption<bool>("FactionChoice.AlignReputations", true);
        cfg.StarterZoneMaxLevel = uint8(sConfigMgr->GetOption<uint32>("FactionChoice.StarterZoneMaxLevel", 4));
        cfg.PromptOnFirstLogin = sConfigMgr->GetOption<bool>("FactionChoice.PromptOnFirstLogin", true);
        cfg.PromptDelay = sConfigMgr->GetOption<uint32>("FactionChoice.PromptDelay", 3000);
        cfg.PromptRetries = uint8(sConfigMgr->GetOption<uint32>("FactionChoice.PromptRetries", 4));
        cfg.PromptRetrySeconds = sConfigMgr->GetOption<uint32>("FactionChoice.PromptRetrySeconds", 20);
        cfg.AllianceCapital  = sConfigMgr->GetOption<std::string>("FactionChoice.Capital.Alliance", "Stormwind");
        cfg.HordeCapital     = sConfigMgr->GetOption<std::string>("FactionChoice.Capital.Horde", "Orgrimmar");

        if (cfg.Enable && !sWorld->getBoolConfig(CONFIG_PLAYER_SETTINGS_ENABLED))
            LOG_WARN("module", "mod-factionchoice: EnablePlayerSettings is off in worldserver.conf, "
                               "faction choices cannot be stored. Module disabled.");

        LOG_INFO("module", "mod-factionchoice: {}", cfg.Enable ? "enabled" : "disabled");
    }

    bool Usable()
    {
        return cfg.Enable && sWorld->getBoolConfig(CONFIG_PLAYER_SETTINGS_ENABLED);
    }

    char const* TeamName(TeamId team)
    {
        return team == TEAM_ALLIANCE ? "Alliance" : "Horde";
    }

    // The faction template the character should be wearing for the chosen side.
    // Characters whose race already belongs to that side keep their own.
    uint32 DesiredFaction(Player* player, TeamId team)
    {
        uint8 const race = Player::TeamIdForRace(player->getRace(true)) == team
            ? player->getRace(true)
            : PROXY_RACE[team];

        ChrRacesEntry const* entry = sChrRacesStore.LookupEntry(race);
        return entry ? entry->FactionID : 0;
    }

    // Something else is intentionally in control of this character's faction.
    bool FactionIsBorrowed(Player* player)
    {
        return player->IsCharmed() || player->HasAuraType(SPELL_AURA_MOD_FACTION);
    }

    TeamId GetOverride(Player* player)
    {
        if (FactionChoiceData const* data = player->CustomData.Get<FactionChoiceData>(DATA_KEY))
            return data->team;

        return TEAM_NEUTRAL;
    }

    void SetOverride(Player* player, TeamId team)
    {
        player->CustomData.GetDefault<FactionChoiceData>(DATA_KEY)->team = team;
    }

    void Enforce(Player* player, TeamId team)
    {
        if (player->GetTeamId() != team)
            player->setTeamId(team);

        if (FactionIsBorrowed(player))
            return;

        uint32 const faction = DesiredFaction(player, team);
        if (faction && player->GetFaction() != faction)
            player->SetFaction(faction);
    }

    uint32 BaselineForTeam(TeamId team)
    {
        return team == TEAM_ALLIANCE ? 1 : 2;
    }

    uint32 RaceMaskForBaseline(Player* player, uint32 baseline)
    {
        if (baseline == 1 || baseline == 2)
            return 1 << (PROXY_RACE[baseline - 1] - 1);

        return player->getRaceMask();
    }

    // Mirrors ReputationMgr::GetBaseReputation and GetDefaultStateFlags, but
    // for an arbitrary race mask, so we can ask what a character of the chosen
    // side would have started with.
    int32 BaselineReputation(FactionEntry const* faction, uint32 raceMask, uint32 classMask, uint32& flags)
    {
        for (uint8 i = 0; i < 4; ++i)
        {
            if ((faction->BaseRepRaceMask[i] & raceMask ||
                    (faction->BaseRepRaceMask[i] == 0 && faction->BaseRepClassMask[i] != 0)) &&
                (faction->BaseRepClassMask[i] & classMask || faction->BaseRepClassMask[i] == 0))
            {
                flags = faction->ReputationFlags[i];
                return faction->BaseRepValue[i];
            }
        }

        flags = 0;
        return 0;
    }

    // Move every reputation from one race's starting values onto another's.
    uint32 MoveReputationBaseline(Player* player, uint32 fromRaceMask, uint32 toRaceMask)
    {
        if (fromRaceMask == toRaceMask)
            return 0;

        ReputationMgr& reputation = player->GetReputationMgr();
        uint32 const classMask = player->getClassMask();
        uint32 changed = 0;

        for (uint32 id = 1; id < sFactionStore.GetNumRows(); ++id)
        {
            FactionEntry const* faction = sFactionStore.LookupEntry(id);
            if (!faction || faction->reputationListID < 0)
                continue;

            uint32 fromFlags = 0;
            uint32 toFlags = 0;
            int32 const fromBase = BaselineReputation(faction, fromRaceMask, classMask, fromFlags);
            int32 const toBase = BaselineReputation(faction, toRaceMask, classMask, toFlags);

            if (fromBase == toBase && (fromFlags & FACTION_FLAG_AT_WAR) == (toFlags & FACTION_FLAG_AT_WAR))
                continue;

            // Changing a standing reveals the faction (SetOneFactionReputation
            // calls SetVisible), so remember whether it was in the reputation
            // pane and put it back the way it was afterwards. Otherwise every
            // faction the baseline happens to touch - including Blizzard's
            // leftovers like REUSE and the test factions - shows up in the pane
            // the moment a character switches sides.
            FactionState const* state = reputation.GetState(faction);
            bool const wasVisible = state && (state->Flags & FACTION_FLAG_VISIBLE);

            // keep what the character earned, swap the baseline underneath it
            int32 const earned = reputation.GetReputation(faction) - fromBase;
            reputation.SetOneFactionReputation(faction, float(toBase + earned), false);
            reputation.AdoptFactionState(faction, wasVisible ? FACTION_FLAG_VISIBLE : 0,
                                         FACTION_FLAG_VISIBLE);

            ++changed;
        }

        return changed;
    }

    // Bring the applied reputation baseline in line with the chosen side.
    uint32 AlignReputations(Player* player, uint32 desiredBaseline)
    {
        if (!cfg.AlignReputations)
            return 0;

        uint32 const applied = player->GetPlayerSetting(SETTINGS_SOURCE, SETTING_REP_BASELINE).value;
        if (applied == desiredBaseline)
            return 0;

        uint32 const changed = MoveReputationBaseline(player,
            RaceMaskForBaseline(player, applied),
            RaceMaskForBaseline(player, desiredBaseline));

        player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_REP_BASELINE, desiredBaseline);
        return changed;
    }

    // Standings persist, but the *flags* do not: ReputationMgr::Initialize()
    // re-derives at-war, hidden and peace-forced from the character's own race
    // on every login, and the core's load path can only undo that for factions
    // that are visible - which the adopted side's are not, for this race.
    // SetVisible refuses on a hidden faction and SetAtWar refuses on a hidden
    // or peace-forced one, so neither can dig a character out of it either.
    //
    // Left alone, a character is put back at war with the side it joined after
    // every relog, and the client then paints those NPCs hostile and will not
    // even open a gossip window with them.
    //
    // So on every login the flags are set to exactly what a character of the
    // chosen side would have: its own cities visible and at peace, the
    // abandoned side's hidden and at war, and everything else merely
    // revealable, so it enters the reputation pane when it is actually met.
    uint32 ReconcileFactionFlags(Player* player, uint32 baseline)
    {
        if (!cfg.AlignReputations)
            return 0;

        ReputationMgr& reputation = player->GetReputationMgr();
        uint32 const raceMask = RaceMaskForBaseline(player, baseline);
        uint32 const classMask = player->getClassMask();
        uint32 changed = 0;

        for (uint32 id = 1; id < sFactionStore.GetNumRows(); ++id)
        {
            FactionEntry const* faction = sFactionStore.LookupEntry(id);
            if (!faction || faction->reputationListID < 0)
                continue;

            FactionState const* state = reputation.GetState(faction);
            if (!state)
                continue;

            // What a character of the chosen side would have been given at
            // creation. No matching row means no flags at all, which is also
            // what such a character would have, so this is not skipped.
            uint32 flags = 0;
            BaselineReputation(faction, raceMask, classMask, flags);

            uint32 wanted = flags & ADOPTED_FLAGS;

            // A faction already in the reputation pane has been met, so it
            // stays there - unless the chosen side hides it outright, which is
            // what the opposite side's cities do for a native character.
            if ((state->Flags & FACTION_FLAG_VISIBLE) &&
                !(wanted & (FACTION_FLAG_HIDDEN | FACTION_FLAG_INVISIBLE_FORCED)))
                wanted |= FACTION_FLAG_VISIBLE;

            if ((state->Flags & ADOPTED_FLAGS) == wanted)
                continue;

            reputation.AdoptFactionState(faction, wanted, ADOPTED_FLAGS);
            ++changed;
        }

        return changed;
    }

    // The state flags only ever reach the client in SMSG_INITIALIZE_FACTIONS,
    // which the core sends from Player::SendInitialPacketsBeforeAddToMap -
    // several hundred lines of WorldSession::HandlePlayerLogin before the
    // OnPlayerLogin hook this module reconciles from. SendStates() is no help:
    // SMSG_SET_FACTION_STANDING carries standings only, and 3.3.5 has no
    // opcode for flags on their own.
    //
    // So the client would keep the at-war state it was handed at load - the
    // character's race's, not the side it joined - and go on refusing to talk
    // to its own faction's NPCs however friendly the server believes they are.
    // Re-sending the whole table is the only way to correct it, and it fixes
    // the reputation pane's contents at the same time.
    void PushFactionFlags(Player* player)
    {
        if (player->GetSession())
            player->GetReputationMgr().SendInitialReputations();
    }

    // Rebasing the standings is not quite enough. The at-war flag can only be
    // changed for factions that are neither hidden nor peace forced
    // (ReputationMgr::SetAtWar bails out on those), and which factions are
    // hidden comes from the character's race - so an Orc that joined the
    // Alliance stays flagged at war with Stormwind no matter what its standing
    // says, which both the server's interaction check and the client's name
    // plate colouring act on.
    //
    // Forced reactions sit in front of both: Unit::GetFactionReactionTo()
    // consults them before anything else, and the client is told about them
    // through SMSG_SET_FORCED_REACTIONS. So wherever the flag could not be
    // corrected, the reaction is pinned instead. They live in memory only and
    // are re-applied on every login.
    uint32 PinReactions(Player* player, uint32 baseline)
    {
        ReputationMgr& reputation = player->GetReputationMgr();
        uint32 const raceMask = RaceMaskForBaseline(player, baseline);
        uint32 const classMask = player->getClassMask();
        uint32 pinned = 0;

        for (uint32 id = 1; id < sFactionStore.GetNumRows(); ++id)
        {
            FactionEntry const* faction = sFactionStore.LookupEntry(id);
            if (!faction || faction->reputationListID < 0)
                continue;

            uint32 flags = 0;
            BaselineReputation(faction, raceMask, classMask, flags);

            bool const wantWar = (flags & FACTION_FLAG_AT_WAR) != 0;
            if (baseline == BASELINE_OWN_RACE || reputation.IsAtWar(faction) == wantWar)
            {
                reputation.ApplyForceReaction(faction->ID, REP_FRIENDLY, false);
                continue;
            }

            reputation.ApplyForceReaction(faction->ID, wantWar ? REP_HOSTILE : REP_FRIENDLY, true);
            ++pinned;
        }

        reputation.SendForceReactions();
        return pinned;
    }

    GameTele const* CapitalFor(TeamId team)
    {
        return sObjectMgr->GetGameTele(team == TEAM_ALLIANCE ? cfg.AllianceCapital : cfg.HordeCapital);
    }

    std::string AreaName(uint32 areaId)
    {
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
            if (char const* name = area->area_name[sWorld->GetDefaultDbcLocale()])
                return name;

        return "your new home";
    }

    struct Destination
    {
        WorldLocation location;
        uint32        areaId = 0;
        std::string   name;
    };

    // Where a character that just switched sides should end up. A capital is
    // no use to a character that has not started its class quests yet, so the
    // low levels are sent to the new side's starting zone instead - taken from
    // playercreateinfo for the side's proxy race, which is where a character
    // of that race would have begun (Northshire Valley or the Valley of
    // Trials, unless those rows were edited).
    struct StartLocation
    {
        uint8         race = 0;
        uint32        areaId = 0;
        WorldLocation location;
        std::string   name;
    };

    // The starting zones of a side, taken from playercreateinfo. Races that
    // share one (dwarves and gnomes) collapse into a single entry.
    void CollectStartLocations(uint8 classId, TeamId team, std::vector<StartLocation>& out)
    {
        for (uint8 race = RACE_HUMAN; race < sRaceMgr->GetMaxRaces(); ++race)
        {
            if (!sChrRacesStore.LookupEntry(race) || Player::TeamIdForRace(race) != team)
                continue;

            PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, classId);
            if (!info)
                continue;

            StartLocation location;
            location.race = race;
            location.location = WorldLocation(info->mapId, info->positionX, info->positionY,
                                              info->positionZ, info->orientation);
            location.areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, location.location);
            location.name = location.areaId ? AreaName(location.areaId) : "an unnamed place";

            // Dwarves and gnomes share a starting zone, so entries are folded
            // together by position rather than by area: an area lookup that
            // came back empty must not collapse the whole list into one.
            bool known = false;
            for (StartLocation const& seen : out)
                if (seen.location.GetMapId() == location.location.GetMapId() &&
                    seen.location.GetExactDist2d(location.location) < 100.0f)
                {
                    known = true;
                    break;
                }

            if (!known)
                out.push_back(std::move(location));
        }
    }

    void SendFactionMenu(Player* player)
    {
        TeamId const natural = Player::TeamIdForRace(player->getRace(true));

        ClearGossipMenuFor(player);
        player->PlayerTalkClass->GetGossipMenu().SetMenuId(MENU_FACTION);

        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "I will fight for the Alliance.", SENDER_FACTION, ACTION_ALLIANCE);
        AddGossipItemFor(player, GOSSIP_ICON_BATTLE, "I will fight for the Horde.", SENDER_FACTION, ACTION_HORDE);
        AddGossipItemFor(player, GOSSIP_ICON_CHAT,
            Acore::StringFormat("Stay with my own people ({}).", TeamName(natural)), SENDER_FACTION, ACTION_KEEP);

        SendGossipMenuFor(player, TEXT_FACTION, player->GetGUID());
    }

    void SendDestinationMenu(Player* player, TeamId team)
    {
        std::vector<StartLocation> locations;
        CollectStartLocations(player->getClass(), team, locations);

        ClearGossipMenuFor(player);
        player->PlayerTalkClass->GetGossipMenu().SetMenuId(MENU_DESTINATION);

        for (StartLocation const& location : locations)
            AddGossipItemFor(player, GOSSIP_ICON_TALK, location.name, SENDER_DESTINATION, location.race);

        if (GameTele const* capital = CapitalFor(team))
            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                Acore::StringFormat("{} (the capital)", capital->name), SENDER_DESTINATION, ACTION_CAPITAL);

        SendGossipMenuFor(player, TEXT_DESTINATION, player->GetGUID());
    }

    // Everything that makes a character belong to a side. Shared by the
    // .faction command and the login prompt.
    void ApplyChoice(Player* player, TeamId team, uint32* rebased = nullptr, uint32* pinned = nullptr)
    {
        player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_TEAM, team == TEAM_ALLIANCE ? 1 : 2);
        SetOverride(player, team);
        Enforce(player, team);

        uint32 const movedReputations = AlignReputations(player, BaselineForTeam(team));
        ReconcileFactionFlags(player, BaselineForTeam(team));
        uint32 const pinnedReactions = cfg.AlignReputations ? PinReactions(player, BaselineForTeam(team)) : 0;

        PushFactionFlags(player);
        player->UpdateObjectVisibility();

        if (rebased)
            *rebased = movedReputations;
        if (pinned)
            *pinned = pinnedReactions;
    }

    // Changing hands the moment a side is picked leaves the character standing
    // in what has just become enemy territory - its own guards turn on it while
    // a second dialog is still open. So the switch waits until the journey is
    // over: pick a side, pick where to start, travel, and only then does the
    // character actually change faction.
    void DeferChoice(Player* player, TeamId team)
    {
        FactionChoiceData* data = player->CustomData.GetDefault<FactionChoiceData>(DATA_KEY);
        data->choosingTeam = TEAM_NEUTRAL;
        data->pendingTeam = team;
        data->applyTimer = 500;
    }

    // Undo everything ApplyChoice did.
    uint32 ClearChoice(Player* player)
    {
        player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_TEAM, 0);
        SetOverride(player, TEAM_NEUTRAL);

        uint32 const restored = AlignReputations(player, BASELINE_OWN_RACE);
        ReconcileFactionFlags(player, BASELINE_OWN_RACE);

        if (cfg.AlignReputations)
            PinReactions(player, BASELINE_OWN_RACE);   // drops the pins

        PushFactionFlags(player);
        player->SetFactionForRace(player->getRace(true));
        player->UpdateObjectVisibility();

        return restored;
    }

    void MoveTo(Player* player, Destination const& destination, ChatHandler& handler)
    {
        if (cfg.SetHomebind)
        {
            player->SetHomebind(destination.location, destination.areaId);
            handler.PSendSysMessage("Your home is now {}.", destination.name);
        }

        if (cfg.TeleportOnSwitch)
        {
            // Your own race's home is hostile now, so move out of it.
            player->TeleportTo(destination.location);
            handler.PSendSysMessage("Moving you to {}.", destination.name);
        }
    }

    bool DestinationFor(Player* player, TeamId team, Destination& out)
    {
        if (cfg.StarterZoneMaxLevel && player->GetLevel() <= cfg.StarterZoneMaxLevel)
        {
            if (PlayerInfo const* info = sObjectMgr->GetPlayerInfo(PROXY_RACE[team], player->getClass()))
            {
                out.location = WorldLocation(info->mapId, info->positionX, info->positionY,
                                             info->positionZ, info->orientation);
                out.areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, out.location);
                out.name = AreaName(out.areaId);
                return true;
            }
        }

        GameTele const* capital = CapitalFor(team);
        if (!capital)
            return false;

        out.location = WorldLocation(capital->mapId, capital->position_x, capital->position_y,
                                     capital->position_z, capital->orientation);
        out.areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, out.location);
        out.name = capital->name;
        return true;
    }
}

class FactionChoice_WorldScript : public WorldScript
{
public:
    FactionChoice_WorldScript() : WorldScript("FactionChoice_WorldScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD }) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadConfig();
    }
};

class FactionChoice_PlayerScript : public PlayerScript
{
public:
    FactionChoice_PlayerScript() : PlayerScript("FactionChoice_PlayerScript",
        {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_UPDATE,
            PLAYERHOOK_ON_UPDATE_FACTION,
            PLAYERHOOK_ON_GOSSIP_SELECT
        }) { }

    void OnPlayerLogin(Player* player) override
    {
        if (!Usable() || !player)
            return;

        SchedulePrompt(player);

        uint32 const stored = player->GetPlayerSetting(SETTINGS_SOURCE, SETTING_TEAM).value;
        if (stored != 1 && stored != 2)
        {
            // No choice (any more): all that can be stale is the reputation
            // baseline, so only that is checked. Reconciling the *flags* here
            // would be wrong as well as noisy - a character on its own race's
            // side already has the flags the core gave it, and forcing the
            // race defaults back over them every login overwrites the player's
            // own at-war choices and re-sends the whole reputation table,
            // which the client reports as a screenful of reputation changes.
            // ClearChoice() reconciles at the moment a side is given up, which
            // is the only time it is actually needed.
            if (AlignReputations(player, BASELINE_OWN_RACE))
                PushFactionFlags(player);

            return;
        }

        TeamId const team = stored == 1 ? TEAM_ALLIANCE : TEAM_HORDE;
        SetOverride(player, team);
        Enforce(player, team);

        // Characters that chose a side before this was implemented get their
        // reputations rebased here, on their next login.
        uint32 const rebased = AlignReputations(player, BaselineForTeam(team));

        // The flags have to be redone every login; the standings do not.
        uint32 const reconciled = ReconcileFactionFlags(player, BaselineForTeam(team));

        // Pins are not persisted either, and are now only needed where a flag
        // could not be reconciled.
        uint32 const pinned = cfg.AlignReputations ? PinReactions(player, BaselineForTeam(team)) : 0;

        // Unconditional: the flags the core already sent the client were taken
        // before any of the above ran.
        PushFactionFlags(player);

        if (rebased || reconciled || pinned)
            LOG_INFO("module", "mod-factionchoice: reputations for {} (rebased {}, flags {}, pinned {})",
                player->GetName(), rebased, reconciled, pinned);

        LOG_INFO("module", "mod-factionchoice: {} (race {}) plays on the {} side",
            player->GetName(), player->getRace(true), TeamName(team));

        if (cfg.AnnounceOnLogin && player->GetSession() && !player->GetSession()->IsBot())
            ChatHandler(player->GetSession()).PSendSysMessage("You are playing on the {} side. Use \".faction reset\" to return to your race's own faction.", TeamName(team));
    }

    // Called from Player::SetFactionForRace, i.e. every time the core resets
    // the character back to its race defaults.
    void OnPlayerUpdateFaction(Player* player) override
    {
        if (!Usable() || !player)
            return;

        TeamId const team = GetOverride(player);
        if (team == TEAM_NEUTRAL)
            return;

        // Only the team here: the core assigns the faction template right
        // after this hook returns, so that part is fixed up on the next update.
        player->setTeamId(team);
    }

    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!cfg.Enable || !player)
            return;

        FactionChoiceData* data = player->CustomData.Get<FactionChoiceData>(DATA_KEY);
        if (!data)
            return;

        // The menu is sent a moment after login, because the client is still
        // finishing its world load - and it silently drops a gossip packet that
        // arrives while it is still on the loading screen. A freshly created
        // character loads slowly enough to lose the first attempt, so wait for
        // the session to stop loading and then try a few times, stopping as
        // soon as the choice is answered (which zeroes the timer).
        if (data->promptTimer)
        {
            if (data->promptTimer > diff)
                data->promptTimer -= diff;
            else if (!player->IsInWorld() || (player->GetSession() && player->GetSession()->PlayerLoading()))
                data->promptTimer = 1000;   // still loading, look again shortly
            else
            {
                SendFactionMenu(player);

                if (++data->promptTries >= cfg.PromptRetries)
                    data->promptTimer = 0;
                else
                    data->promptTimer = cfg.PromptRetrySeconds * IN_MILLISECONDS;

                LOG_INFO("module", "mod-factionchoice: offered the faction choice to {} (attempt {})",
                    player->GetName(), data->promptTries);
            }
        }

        // A deferred switch lands here once the character has actually arrived:
        // still being teleported, or still loading, means wait a little longer.
        if (data->pendingTeam != TEAM_NEUTRAL)
        {
            if (data->applyTimer > diff)
                data->applyTimer -= diff;
            else if (!player->IsInWorld() || player->IsBeingTeleported() ||
                     (player->GetSession() && player->GetSession()->PlayerLoading()))
                data->applyTimer = 500;
            else
            {
                TeamId const team = data->pendingTeam;
                data->pendingTeam = TEAM_NEUTRAL;
                data->applyTimer = 0;

                ApplyChoice(player, team);
                player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_PROMPTED, 1);

                if (player->GetSession() && !player->GetSession()->IsBot())
                    ChatHandler(player->GetSession()).PSendSysMessage("You now fight for the {}.", TeamName(team));

                LOG_INFO("module", "mod-factionchoice: {} (race {}) took up the {} banner on arrival",
                    player->GetName(), player->getRace(true), TeamName(team));
            }
        }

        if (data->team == TEAM_NEUTRAL)
            return;

        Enforce(player, data->team);
    }

    void OnPlayerGossipSelect(Player* player, uint32 menuId, uint32 sender, uint32 action) override
    {
        if (!Usable() || !player || !player->GetSession())
            return;

        ChatHandler handler(player->GetSession());

        if (menuId == MENU_FACTION && sender == SENDER_FACTION)
        {
            CloseGossipMenuFor(player);

            if (FactionChoiceData* data = player->CustomData.Get<FactionChoiceData>(DATA_KEY))
                data->promptTimer = 0;   // answered, stop re-offering

            TeamId const natural = Player::TeamIdForRace(player->getRace(true));

            if (action == ACTION_KEEP)
            {
                if (GetOverride(player) != TEAM_NEUTRAL)
                    ClearChoice(player);

                player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_PROMPTED, 1);

                handler.PSendSysMessage("You stay with the {}. \".faction horde\" or \".faction alliance\" will still change your mind later.", TeamName(natural));
                return;
            }

            TeamId const team = action == ACTION_ALLIANCE ? TEAM_ALLIANCE : TEAM_HORDE;

            // Staying with your own people needs no journey and no waiting.
            if (team == natural)
            {
                ApplyChoice(player, team);
                player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_PROMPTED, 1);
                handler.PSendSysMessage("You now fight for the {}.", TeamName(team));
                return;
            }

            // Swapping sides: choose where to start first. The faction itself
            // changes on arrival, so the character is not left hostile in its
            // own starting zone while this second window is open.
            player->CustomData.GetDefault<FactionChoiceData>(DATA_KEY)->choosingTeam = team;
            SendDestinationMenu(player, team);
            return;
        }

        if (menuId == MENU_DESTINATION && sender == SENDER_DESTINATION)
        {
            CloseGossipMenuFor(player);

            FactionChoiceData* data = player->CustomData.Get<FactionChoiceData>(DATA_KEY);

            // Mid-flow the side has been picked but not applied yet, so the
            // choice comes from choosingTeam; ".faction where" instead re-opens
            // this window for a side already taken.
            TeamId const team = data && data->choosingTeam != TEAM_NEUTRAL ? data->choosingTeam : GetOverride(player);
            if (team == TEAM_NEUTRAL)
                return;

            Destination destination;

            if (action == ACTION_CAPITAL)
            {
                GameTele const* capital = CapitalFor(team);
                if (!capital)
                    return;

                destination.location = WorldLocation(capital->mapId, capital->position_x,
                                                     capital->position_y, capital->position_z,
                                                     capital->orientation);
                destination.areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, destination.location);
                destination.name = capital->name;
            }
            else
            {
                PlayerInfo const* info = sObjectMgr->GetPlayerInfo(uint8(action), player->getClass());
                if (!info || Player::TeamIdForRace(uint8(action)) != team)
                    return;

                destination.location = WorldLocation(info->mapId, info->positionX, info->positionY,
                                                     info->positionZ, info->orientation);
                destination.areaId = sMapMgr->GetAreaId(PHASEMASK_NORMAL, destination.location);
                destination.name = AreaName(destination.areaId);
            }

            MoveTo(player, destination, handler);

            // Only a character that has not switched yet needs the deferral;
            // ".faction where" is just a relocation.
            if (data && data->choosingTeam != TEAM_NEUTRAL)
            {
                DeferChoice(player, team);
                handler.PSendSysMessage("You take up the {} banner once you arrive.", TeamName(team));
            }
        }
    }

private:
    static void SchedulePrompt(Player* player)
    {
        if (!cfg.PromptOnFirstLogin)
            return;

        // Bots have no one to answer the window, and a character that already
        // answered is never asked again.
        if (!player->GetSession() || player->GetSession()->IsBot())
            return;

        if (player->GetPlayerSetting(SETTINGS_SOURCE, SETTING_PROMPTED).value)
            return;

        // A character that already has a side (picked with .faction before
        // the prompt existed) has nothing to answer.
        if (player->GetPlayerSetting(SETTINGS_SOURCE, SETTING_TEAM).value)
        {
            player->UpdatePlayerSetting(SETTINGS_SOURCE, SETTING_PROMPTED, 1);
            return;
        }

        player->CustomData.GetDefault<FactionChoiceData>(DATA_KEY)->promptTimer =
            std::max<uint32>(cfg.PromptDelay, 1);
    }
};

class FactionChoice_CommandScript : public CommandScript
{
public:
    FactionChoice_CommandScript() : CommandScript("FactionChoice_CommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable factionChoiceCommandTable =
        {
            { "alliance", HandleFactionAllianceCommand, SEC_PLAYER, Console::Yes },
            { "horde",    HandleFactionHordeCommand,    SEC_PLAYER, Console::Yes },
            { "reset",    HandleFactionResetCommand,    SEC_PLAYER, Console::Yes },
            { "status",   HandleFactionStatusCommand,   SEC_PLAYER, Console::Yes },
            { "where",    HandleFactionWhereCommand,    SEC_PLAYER,     Console::Yes },
            { "zones",    HandleFactionZonesCommand,    SEC_GAMEMASTER, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "faction", factionChoiceCommandTable }
        };

        return commandTable;
    }

    static bool HandleFactionAllianceCommand(ChatHandler* handler)
    {
        return Switch(handler, TEAM_ALLIANCE);
    }

    static bool HandleFactionHordeCommand(ChatHandler* handler)
    {
        return Switch(handler, TEAM_HORDE);
    }

    static bool HandleFactionResetCommand(ChatHandler* handler)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        uint32 const rebased = ClearChoice(player);

        if (rebased)
            handler->PSendSysMessage("Restored {} reputations to your race's starting values.", rebased);

        handler->PSendSysMessage("Faction reset to your race's own faction ({}).",
            TeamName(Player::TeamIdForRace(player->getRace(true))));
        return true;
    }

    // Re-opens the destination chooser, so the starting zone can be picked by
    // hand rather than taking the level based default.
    static bool HandleFactionWhereCommand(ChatHandler* handler)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        TeamId const team = GetOverride(player);
        if (team == TEAM_NEUTRAL)
        {
            handler->SendErrorMessage("You are playing your own race's faction, so there is nothing to choose.");
            return false;
        }

        if (player->IsInCombat())
        {
            handler->SendErrorMessage("Not while you are in combat.");
            return false;
        }

        SendDestinationMenu(player, team);
        return true;
    }

    // Prints what the destination chooser would offer. Console friendly, so
    // the list can be checked without logging in.
    static bool HandleFactionZonesCommand(ChatHandler* handler, Optional<std::string> sideArg)
    {
        std::string side = sideArg ? *sideArg : "alliance";
        std::transform(side.begin(), side.end(), side.begin(), ::tolower);

        TeamId team;
        if (side == "alliance" || side == "a")
            team = TEAM_ALLIANCE;
        else if (side == "horde" || side == "h")
            team = TEAM_HORDE;
        else
        {
            handler->SendErrorMessage("Usage: .faction zones alliance|horde");
            return false;
        }

        std::vector<StartLocation> locations;
        CollectStartLocations(CLASS_WARRIOR, team, locations);

        handler->PSendSysMessage("{} starting zones offered to a character that switched sides:", TeamName(team));

        for (StartLocation const& location : locations)
            handler->PSendSysMessage("  {} (race {}, map {}, area {}) at {:.1f} {:.1f} {:.1f}",
                location.name, location.race, location.location.GetMapId(), location.areaId,
                location.location.GetPositionX(), location.location.GetPositionY(),
                location.location.GetPositionZ());

        if (GameTele const* capital = CapitalFor(team))
            handler->PSendSysMessage("  plus the capital: {} (map {})", capital->name, capital->mapId);

        return true;
    }

    static bool HandleFactionStatusCommand(ChatHandler* handler)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        TeamId const chosen = GetOverride(player);
        TeamId const natural = Player::TeamIdForRace(player->getRace(true));

        if (chosen == TEAM_NEUTRAL)
            handler->PSendSysMessage("Faction: {} (your race's own faction, no override).", TeamName(natural));
        else
            handler->PSendSysMessage("Faction: {} (chosen; your race's own faction is {}).",
                TeamName(chosen), TeamName(natural));

        uint32 const baseline = player->GetPlayerSetting(SETTINGS_SOURCE, SETTING_REP_BASELINE).value;
        handler->PSendSysMessage("Reputations: {}.", baseline == BASELINE_OWN_RACE
            ? "your race's own starting values"
            : (baseline == 1 ? "Alliance starting values" : "Horde starting values"));

        return true;
    }

private:
    static Player* Caller(ChatHandler* handler)
    {
        Player* player = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
        if (!player)
        {
            handler->SendErrorMessage("This command can only be used in game.");
            return nullptr;
        }

        if (!Usable())
        {
            handler->SendErrorMessage("Faction choice is disabled on this server.");
            return nullptr;
        }

        return player;
    }

    static bool Switch(ChatHandler* handler, TeamId team)
    {
        Player* player = Caller(handler);
        if (!player)
            return false;

        // Switching sides mid fight, mid battleground or inside an instance
        // would leave the character on the wrong side of an encounter.
        if (player->IsInCombat())
        {
            handler->SendErrorMessage("You cannot change faction while in combat.");
            return false;
        }

        if (player->InBattleground() || player->InArena())
        {
            handler->SendErrorMessage("You cannot change faction inside a battleground or arena.");
            return false;
        }

        if (Map* map = player->GetMap())
        {
            if (map->IsDungeon())
            {
                handler->SendErrorMessage("You cannot change faction inside an instance.");
                return false;
            }
        }

        if (GetOverride(player) == team && Player::TeamIdForRace(player->getRace(true)) == team)
        {
            handler->SendErrorMessage("You are already on that side.");
            return false;
        }

        Destination destination;
        bool const travelling = (cfg.TeleportOnSwitch || cfg.SetHomebind) && DestinationFor(player, team, destination);

        // Travel first when there is somewhere to go: changing hands on the
        // spot would leave the character hostile where it stands.
        if (travelling)
        {
            MoveTo(player, destination, *handler);
            DeferChoice(player, team);
            handler->PSendSysMessage("You take up the {} banner once you arrive.", TeamName(team));
            handler->PSendSysMessage("Use \".faction where\" later if you would rather pick the spot yourself.");
            return true;
        }

        // ApplyChoice also rebases reputations: without that the new side's
        // NPCs stay unfriendly, because they judge you by your reputation
        // rather than by your faction template.
        uint32 rebased = 0;
        uint32 pinned = 0;
        ApplyChoice(player, team, &rebased, &pinned);

        handler->PSendSysMessage("You now fight for the {}.", TeamName(team));

        if (rebased)
            handler->PSendSysMessage("Rebased {} reputations onto the {} starting values.", rebased, TeamName(team));

        if (pinned)
            handler->PSendSysMessage("Pinned the reaction of {} factions that cannot have their war state changed.", pinned);

        if (cfg.TeleportOnSwitch || cfg.SetHomebind)
            handler->PSendSysMessage("Could not work out where to send you, so you were not moved.");

        return true;
    }
};

void AddFactionChoiceScripts()
{
    new FactionChoice_WorldScript();
    new FactionChoice_PlayerScript();
    new FactionChoice_CommandScript();
}
