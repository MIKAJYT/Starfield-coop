# Co-op spec: status and plan

The owner's co-op spec, item by item: what PULSAR Revived does today, what is missing,
and in which phase it gets done. **Status** words:

- **done:** implemented and passing an automated bot run (and its twin, where one exists).
- **exists:** implemented (mostly by the original PULSAR or the Pinganiyo fork) but not
  tested by a bot yet. Each gets a bot scenario in the phase listed.
- **partial:** some of it exists; the gap is named.
- **missing:** not started.

Last updated 2026-10-30. Run IDs and details are in the working notes
(coop-anything `games/starfield/NOTES.md`).

## Core tech

| Spec item | Status | Today | Phase |
|---|---|---|---|
| Host-authority netcode with stable lobby (create/join, reconnect) | partial | Host-authoritative for world state; NPC firing is not host-authoritative. Lobby and join codes go through the original author's lobby server and relay (funded until end of 2026). Reconnect after a drop: bot `reconnect` (25 s network outage, back by itself in 31 s), twin `AutoReconnect = 0` fails. | 2 (no relay, codes without a server), 4 |
| Full player sync: run, sprint, crouch, jump, jetpack, aim, animations, outfits | partial | Run, sprint, sneak, jump, armor and weapons on the partner's figure exist. Aim: the up/down pitch and the sights pose arrive (bot `aim`); the sights pose now ends with the aim (it lingered 8 s). Whether the figure's head visibly turns is not proven: the bot's head-bone reading stays fixed once a weapon is drawn, in both the run and its twin. Jetpack: the figure now fires its own boostpack while the partner boosts (`CoopJetpackSync`); the bot can't lift the host with the engine's boost actions, so this is untested. Players 3-4: no aim pitch (no look marker). | 4 (rest needs a person or a better bot) |
| Combat sync: shooting, hits, damage, downed/death, revive, NPC damage | exists | Fire actions, NPC damage and deaths, downed and revive exist; loot kill tested. NPC damage bot-tested both ways (`damage`, twin `CoopNpcHealthSync = 0` fails). Only tracked NPCs (enemies, companions, guards, anyone in combat) share health; city civilians share only their position. | 4 (done) |
| World/travel: fast travel, planet loading, POI/dungeon entry | partial | Each player fast travels alone, F8 regroups. Stations and POIs differing between players in space: not reproduced with a real arrival. From a save the owner made at The Eye after a star-map jump (station in view, dockable), host and client both load into the orbit cell and both draw the station (`TheEyeREF` with 3D on both, run 20261031-112524). The likely cause of the old "missing POIs, stuck in the ship" reports was the lost save transfer (fixed in phase 4): the client never got the host's save and stayed behind. Bot `space` moves the ship with Papyrus `MoveTo` (star-map travel has no script call) and so parks it inside the station's close-up cell, which a player only reaches by docking; there a load leaves the close-up pieces unloaded on every machine, the host too (run 20261031-110120). Its FAIL is that artifact, not the bug; kept as a census tool. | 5 |
| Spaceflight: launch, cruise, manual flight, docking, station entry; ship position | partial | Launch done (vote). Flight input is mirrored to the partner. Ship position sync is switched off in the code (`kShipPosSyncEnabled = false`: the ship object does not move in flight). Docking: the host's dock reaches the client, which rewires its cockpit hatch to the station's (`DockSync`, switch `CoopDockSync`); the client's ship does not report docked itself. Bot `dock` from a save the owner made docked at The Eye (real docking sequence): host and client both walk through the cockpit hatch and arrive in the station cell 0001285B (run 20261031-131445); the twin passes too, since the client loads the host's save already docked: docking before a partner joins needs no sync. Docking while both are in the session works, tested with the owner docking by hand (bot `dock`, `ManualDock = 1`, run 20261031-143656): the host walks through the hatch into The Eye; the client's ship never reports docked itself, but `DockSync` rewires its hatch, and after standing up the client arrives in the same station cell and sees the host. No twin run yet (needs a person docking again). Bots cannot do the docking sequence (`InstantDock` skips it, run 20261031-121346) or stand the client up from its seat. Found with it: a client that joins while the host pilots started in the pilot seat, overlapping the host's figure. Now (`CoopLeaveHostSeat`) it snaps into the nearest free seat or chair (Papyrus `SnapIntoInteraction`), else tries to stand up. Bot `seat`: moves from the pilot seat into a chair 4.7 m away; twin stays in the pilot seat. Watched by the owner: the client sits in the chair and sees the host. Fixed with it: an old rule hid the other player's figure whenever the local player sat anywhere; now only in the seat the client loaded into, and the host no longer hides the client (not yet watched). Open: the host's figure stands up a few seconds late on the client (the host reports the stand-up when its animation ends, then the client plays its own and respawns the figure). | 6 |
| Quest/event sync: quest states, triggers, scripted events (doors, cutscenes, interactions) | partial | Quest stages, doors, locks, activators and dialogue exist. Scenes (cutscenes): every scene the host's game starts or ends is sent to the clients, which start it (its quest first when needed) or stop it (`CoopSceneSync`). Bot `scene`: the host starts MQ103_03_ArriveAkilaScene, the client plays it 90 ms later; twin `CoopSceneSync = 0` fails. Limits: background scenes whose quest only the game's story manager may start (e.g. an NPC raking) can't be forced onto a client where that quest isn't running; a scene with a player part may pull the client's own character in (untested). | 5 (scenes done) |
| Stability: SFSE + Address Library, crash logging, installer/launcher, lobby UI, crash reports, character reset | partial | SFSE and Address Library yes. Crash guard absorbs known engine crashes, but no crash dumps or reports are written. The launcher (installer, lobby, character backup and reset) is the original C# tool. | 2 (launcher join codes), 7 (crash dumps and reports) |
| Target: 2-4 players, PC Steam, clear versions | partial | Versions documented (COMPATIBILITY.md). Up to 4 players connect (`MaxPlayers`); the host forwards between clients; every player has a figure on every screen. Bot-tested with 2 game copies plus a headless third player. Takeoff vote, quest rewards, downed/revive, item drops, kill XP and the pilot's flight data work for every player (phase 3c). Voice is still first partner only. | 3 |

## Loot, inventory and progression

| Spec item | Status | Today | Phase |
|---|---|---|---|
| Enemy loot: each player gets their own copy | done | Bot `loot`, twin fails. | 1 |
| World items shared, first to take removes it for all | done | Bot `pickup` (host takes a loose item, gone on the client), twin `CoopSharedWorldItems = 0` fails. Containers not bot-tested. | 4 |
| Quest reward items for everyone | done | Bot `reward`, twin fails. Quest XP and weapon mods not yet. | 1, rest in 8 |
| Separate inventories | exists | Each player's own character is restored over the host's save. Bot-tested (`separate`): after a re-bridge each player has only their own item changes; twin `CoopRestoreOwnCharacter = 0` fails. | 4 (done) |
| Separate skill trees | exists | Level, skills and perks are part of the restored character. Bot-tested in the same run: own perk rank and skill points kept, the other player's not taken over. | 4 (done) |

## Ships and movement

| Spec item | Status | Today | Phase |
|---|---|---|---|
| Fly together in the same ship, multi-crew (pilot, gunner, engineer) | partial | The partner comes along in the cockpit as a passenger. Roles: missing. | 6 |
| Doors move only the user | done | Bot `door`, twin fails. | 1 |
| Takeoff vote; everyone moved aboard; then takeoff | done | Bot `takeoff` with 2 and 3 players (the launch waits for the last ready player), also with the partner outside the ship; twins fail. The bot now opens the takeoff menu as a pilot does (the phase 1 test bypassed the hold). | 1, 3 |

## Co-op gameplay

| Spec item | Status | Today | Phase |
|---|---|---|---|
| One team: shared objectives and rewards, no PvP, no player economy | done | Quest stages and rewards shared. No friendly fire: bot `friendlyfire` (a lethal hit on the partner's figure, healed within 2.5 s, the real player untouched); twin `CoopFriendlyFireGuard = 0` fails (the figure stayed at -31 health, which used to put it into the downed pose on that screen). No player economy exists. | 4 |
| Joint missions (faction missions, outpost defense, rescues) | partial | Rides on quest sync. A quest the host starts now starts on the clients too (`CoopQuestStartSync`): before, the client only logged the start and the stage worker skipped every later stage of a quest not running there, so a quest picked up mid-session never reached the client. Bot `quest`: the host starts City_AkilaLife05, the client 62 ms later; twin fails. Mission-board missions (bounties, cargo, rescues, outpost defense) are started by the game's story manager with randomly picked targets, which no script can start and which differ per machine. When the host sets a stage in such a quest the client doesn't run (accepting a mission), the client asks for the host's save, which holds the mission with the host's targets (`CoopMissionBridge`); costs the client one loading screen per accepted mission, browsing a board costs nothing. Bot `mission`: the client lacks MB_Bounty01Far, the host moves it from stage 5 to 10, the client asks, gets the save 2.3 s later and runs the mission at stage 10 with its own character restored; twin fails. The bot checks the quest and stage, not each target. | 5 (done) |
| Shared outposts with team build/remove/storage permissions | missing | | 8 |
| Ship roles, repair and damage systems | partial | Ship damage and repair: the host's hull is mirrored onto the client's ship (hits, repairs, destruction of both). Bot `hull` (from a save in the ship at The Eye): the host's ship 408 -> 244.8 of 948 and back to 408, the client follows each within 0.9 s; twin `CoopShipHullSync = 0` stays at 408. Roles (gunner, engineer) and a client-side repair: missing. | 6 |
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
   Done: friendly fire, world pickups, reconnect, aim (partly), separate inventories and
   skill trees, NPC damage both ways. Left: jetpack and head pitch proof (need a bot that can
   fly and look, or a person). Lost save transfer, fixed: a mid-session save from the host
   twice never reached the client (`separate` twin 20261030-212343; `space`
   20261031-014412, where the client stayed in orbit while the host was at The Eye). The
   host's network thread sent the save without the ENet lock while the game thread sent
   too; ENet is not thread-safe. Now locked. Bot `bridges` (6 save bridges 45 s apart):
   before, about 2 of 11 mid-session saves were lost; after, 12 of 12 arrived in two runs.
5. **World and quests:** POI/station divergence, cutscenes, mission types.
   Done: scene sync (bot `scene`), quest start sync (bot `quest`), mission-board missions
   through the host's save (bot `mission`). Station/POI divergence: not reproduced with a
   real arrival (the owner's save at The Eye: both draw the station); the bot `space` gap
   (client 0 of 3895 close-up pieces drawn) comes from its `MoveTo` shortcut, which puts
   the ship inside the station's close-up cell; the host loses those pieces there too on a
   plain load. Probable real cause of the reports: the lost save transfer, fixed. The
   Creation Kit's base scripts (Tools/ContentResources.zip) are extracted to
   instances/papyrus-base for script signatures and compiling.
6. **Ships:** ship position sync, docking, crew roles (pilot, gunner, engineer), repair.
7. **Stability:** crash dumps and reports, Community Patch test, launcher join codes.
8. **Team systems:** shared outposts with permissions, quest XP sharing, team buffs and
   role perks.

Each feature gets a bot scenario with a twin, as in phase 1. Features that can't be
tested by bots (a real internet connection between two homes, people playing) are named
as untested until someone plays.
