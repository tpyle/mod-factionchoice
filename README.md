# mod-factionchoice

Lets a character play any race on either faction.

A newly created character is asked, once, which side it wants to fight for.
Choosing the other side rewrites the character's team: faction standings,
war states, the reputation pane, homebind and, optionally, a teleport to the
other side's capital, so a Tauren paladin can walk into Stormwind and be
served.

## Configuration (`mod_factionchoice.conf`)

`FactionChoice.Enable`, `.PromptOnFirstLogin`, `.PromptDelay`,
`.PromptRetries`, `.PromptRetrySeconds`, `.TeleportOnSwitch`,
`.SetHomebind`, `.Capital.Alliance`, `.Capital.Horde`, `.AlignReputations`,
`.StarterZoneMaxLevel`, `.AnnounceOnLogin`.

## Commands

    .faction status            what side this character is on
    .faction alliance|horde    switch it
    .faction reset             back to the race's own side
    .faction where             where the character would be sent
    .faction zones             the starter zones the switch understands

## Requirements

A core with `ReputationMgr::AdoptFactionState`. `SetVisible()` and
`SetAtWar()` refuse on a faction the character's own race has hidden or
peace-forced, which is right for an ordinary character and wrong here:
`Initialize()` re-derives those flags from the race on every login, so
without it the adopted side comes back at war after each relog and the client
refuses to talk to its own faction's NPCs.

## Licence

GNU Affero General Public License v3.0, the licence AzerothCore and its
modules use. See [LICENSE](LICENSE).
