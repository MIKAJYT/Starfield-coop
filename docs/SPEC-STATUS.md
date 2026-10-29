# Co-op spec: status and plan

The owner's co-op spec, item by item: what PULSAR Revived does today, what is missing,
and in which phase it gets done. **Status** words:

- **done:** implemented and passing an automated bot run (and its twin, where one exists).
- **exists:** implemented (mostly by the original PULSAR or the Pinganiyo fork) but not
  tested by a bot yet. Each gets a bot scenario in the phase listed.
- **partial:** some of it exists; the gap is named.
- **missing:** not started.

Last updated 2026-10-29. Run IDs and details are in the working notes
(coop-anything `games/starfield/NOTES.md`).

## Core tech

| Spec item | Status | Today | Phase |
|---|---|---|---|
| Host-authority netcode with stable lobby (create/join, reconnect) | partial | Host-authoritative for world state; NPC firing is not host-authoritative. Lobby and join codes go through the original author's lobby server and relay (funded until end of 2026). Reconnect after a short drop exists. | 2 (no relay, codes without a server), 4 (reconnect bot test) |
| Full player sync: run, sprint, crouch, jump, jetpack, aim, animations, outfits | partial | Run, sprint, sneak, jump, look pitch, armor and weapons on the partner's figure exist. Jetpack: missing. Aim: only "aiming" on/off. | 4 |
| Combat sync: shooting, hits, damage, downed/death, revive, NPC damage | exists | Fire actions, NPC damage and deaths, downed and revive exist; loot kill tested. | 4 |
| World/travel: fast travel, planet loading, POI/dungeon entry | partial | Each player fast travels alone, F8 regroups. Stations and POIs can differ between players in space (known bug). | 5 |
| Spaceflight: launch, cruise, manual flight, docking, station entry; ship position | partial | Launch done (vote). Flight input is mirrored to the partner. Ship position sync is switched off in the code (`kShipPosSyncEnabled = false`). Docking exists, rough. | 6 |
| Quest/event sync: quest states, triggers, scripted events (doors, cutscenes, interactions) | partial | Quest stages, doors, locks, activators and dialogue exist. Cutscenes: missing. | 5 |
| Stability: SFSE + Address Library, crash logging, installer/launcher, lobby UI, crash reports, character reset | partial | SFSE and Address Library yes. Crash guard absorbs known engine crashes, but no crash dumps or reports are written. The launcher (installer, lobby, character backup and reset) is the original C# tool. | 2 (launcher join codes), 7 (crash dumps and reports) |
| Target: 2-4 players, PC Steam, clear versions | partial | Versions documented (COMPATIBILITY.md). **2 players only.** | 3 |

## Loot, inventory and progression

| Spec item | Status | Today | Phase |
|---|---|---|---|
| Enemy loot: each player gets their own copy | done | Bot `loot`, twin fails. | 1 |
| World items shared, first to take removes it for all | exists | Container takes and world pickups are mirrored. | 4 (bot test) |
| Quest reward items for everyone | done | Bot `reward`, twin fails. Quest XP and weapon mods not yet. | 1, rest in 8 |
| Separate inventories | exists | Each player's own character is restored over the host's save. | 4 (bot test) |
| Separate skill trees | exists | Level, skills and perks are part of the restored character. | 4 (bot test) |

## Ships and movement

| Spec item | Status | Today | Phase |
|---|---|---|---|
| Fly together in the same ship, multi-crew (pilot, gunner, engineer) | partial | The partner comes along in the cockpit as a passenger. Roles: missing. | 6 |
| Doors move only the user | done | Bot `door`, twin fails. | 1 |
| Takeoff vote; everyone moved aboard; then takeoff | done | Bot `takeoff`, also with the partner outside the ship; twin fails. | 1 (3-4 players in 3) |

## Co-op gameplay

| Spec item | Status | Today | Phase |
|---|---|---|---|
| One team: shared objectives and rewards, no PvP, no player economy | partial | Quest stages and rewards shared; players can't hurt each other's figure. No player economy exists. Needs a bot check for friendly fire. | 4 |
| Joint missions (faction missions, outpost defense, rescues) | partial | Rides on quest sync; untested per mission type. | 5 |
| Shared outposts with team build/remove/storage permissions | missing | | 8 |
| Ship roles, repair and damage systems | missing | | 6 |
| Fair XP and team synergies (team buffs, role perks) | partial | Kill XP is shared. Quest XP, team buffs and role perks: missing. | 8 |

## Org and tech

| Spec item | Status | Today | Phase |
|---|---|---|---|
| SFSE, Address Library, ideally Community Patch; load order and incompatibilities | done (docs) | COMPATIBILITY.md. Community Patch untested. | 1 (patch test in 7) |
| Target FPS, max players, bandwidth and tick rate | done (docs) | PERFORMANCE.md, measured. | 1 |
| Private sessions, direct P2P, no relay, no port forwarding | partial | Join codes (`DirectSession = 1`): LAN, STUN hole punching and UPnP, reply code when needed, session secret in the key exchange. Bot: join by code PASS, stale-code twin FAIL. STUN checked against the real servers (public address found, port kept). Launcher card "Play with friends, no server": host gets a code to copy, friend pastes it, reply codes go both ways; the reply-code handover is bot-tested, the launcher screen itself is not. Not yet tested: a router's UPnP (owner declined for now), a connection between two homes. | 2 |

## Phases

1. **Done:** doors, enemy loot, quest rewards, takeoff vote, docs, host cost.
2. **Direct connection without relay or port forwarding.** In order of what is tried:
   IPv6 direct; automatic port opening on the host's router (UPnP/NAT-PMP, the router
   feature many games use); UDP hole punching with endpoints learned from a public STUN
   server (it only tells each PC its public address, it carries no game traffic). Join by a
   code the host sends to a friend; if that alone can't connect, the friend sends a reply
   code back. **Limit:** when both players sit behind carrier-grade or symmetric NAT
   (some mobile and fibre providers), no method without a relay can connect them; the
   game will say so plainly.
3. **2-4 players.** The network layer holds one peer today; every module assumes one
   partner and one partner figure. Host becomes a hub for up to 3 clients, each with their
   own figure, vote, rewards and loot.
4. **Bot tests for everything that "exists"**, then jetpack and aim sync, friendly fire off.
5. **World and quests:** POI/station divergence, cutscenes, mission types.
6. **Ships:** ship position sync, docking, crew roles (pilot, gunner, engineer), repair.
7. **Stability:** crash dumps and reports, Community Patch test, launcher join codes.
8. **Team systems:** shared outposts with permissions, quest XP sharing, team buffs and
   role perks.

Each feature gets a bot scenario with a twin, as in phase 1. Features that can't be
tested by bots (a real internet connection between two homes, people playing) are named
as untested until someone plays.
