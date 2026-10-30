# Changelog

Changes in this fork (PULSAR Revived), based on PULSAR 0.3.0 by
ramisotti13-eng. Both players must run the same version.

**How to read this:** every entry says how it was checked.
- *Tested:* covered by the automated tests (`pulsar_tests.exe`, no game needed).
- *Not tested in game:* builds and passes the automated tests, but nobody has
  played it yet. Treat these as fixes to verify, not proven fixes.
- *Bot-tested:* passed an automated co-op run with two real game copies on one PC
  (`tests/coopqa/Run-CoopQA.ps1`, see [docs/TESTING.md](docs/TESTING.md)), and its twin
  run (the feature switched off) failed, so the test can tell the difference. Not
  played by people yet.

The roadmap goals in the README (for example "0.7.0: a two-player campaign
without major workarounds") are only met once verified in game. A version
number here means the code for that milestone is in, not that the goal is met.

## Unreleased - full co-op spec, phase 5: world and quests

- **Scene sync, cutscenes** (`CoopSceneSync`, default 1). A scene the host's game starts
  (by a trigger, a dialogue line, an object or a script) used to play only there unless
  a quest stage started it. The host now sends every scene start and end; the client
  starts the same scene, its quest first when that isn't running, or stops it. The
  engine's scene event has no address in CommonLibSF; PULSAR finds it at start by its
  vtable. *Bot-tested* (`scene`): the host starts MQ103_03_ArriveAkilaScene, the client
  plays it 90 ms later; twin with the switch off never does. Background scenes whose
  quest only the game may start can't be forced onto a client where that quest isn't
  running. A scene with a player part is not tested.
- **Quests the host starts now start for the client** (`CoopQuestStartSync`, default
  1). The client used to only log the host's quest start, and the game's stage worker
  skips quests that aren't running, so every stage of a quest picked up mid-session was
  lost on the client. Now the client starts the quest (Papyrus `Quest.Start`) and then
  applies the stage. *Bot-tested* (`quest`): the host starts City_AkilaLife05, the
  client starts it 62 ms later; twin with the switch off never does.
- **Mission-board missions** (`CoopMissionBridge`, default 1). The story manager starts
  them with targets picked at random per machine, and no script can start them on the
  client. When the host sets a stage in one the client doesn't run (accepting it), the
  client asks for the host's save, which holds the mission with the host's targets. One
  loading screen for the client per accepted mission; browsing a board costs nothing.
  *Bot-tested* (`mission`): the client lacks MB_Bounty01Far, the host moves it to stage
  10, the client gets the host's save 2.3 s after asking and runs the mission at stage 10
  with its own character restored; twin with the switch off stays without it.
- **Space census bot** (`space`): after takeoff each bot lists everything in space
  around the ship and the launcher compares the lists. Groundwork for the station/POI
  mismatch; right after takeoff the test orbit is empty, so it needs a flight to a
  station first.

## Unreleased - full co-op spec, phase 4: tests for what exists, aim, jetpack, no friendly fire

- **No friendly fire** (`CoopFriendlyFireGuard`, default 1). A hit on a partner's figure
  took its health down and at 0 put it into the downed pose on that screen while the real
  player was fine. Figures now get their health back at once (unless that player is really
  downed). *Bot-tested* (`friendlyfire`); twin with the guard off fails.
- **Reconnect** (`AutoReconnect`, default 1). *Bot-tested*: a 25 s network outage on the
  client, the session comes back by itself 6 s after it ends; twin stays disconnected.
- **Shared world items** (`CoopSharedWorldItems`, default 1). *Bot-tested*: an item the
  host picks up disappears on the client; twin keeps it there.
- **Aim**: the partner's figure left the sights pose 8 s after they stopped aiming; now
  0.5 s. `CoopAimSync = 0` turns aim sync off. *Bot run*: pitch and sights pose arrive;
  a visible head turn is not proven yet.
- **Jetpack** (`CoopJetpackSync`, default 1): while the partner boosts (jump state "boost"
  for 250 ms), their figure fires its own boostpack. *Not tested in game*: the bot's
  boost actions animate but don't lift the player.
- **Separate inventories and skill trees** (`CoopRestoreOwnCharacter`, default 1).
  *Bot-tested* (`separate`): both players change an item, a perk rank and skill points,
  the host re-sends its save, and each player keeps only their own changes. Twin with the
  switch off fails (the client takes over the host's character).
- **NPC damage both ways** (`CoopNpcHealthSync`, default 1). *Bot-tested* (`damage`): the
  host hits one tracked NPC, the client another; each hit shows on the other screen within
  15 s. Twin with the switch off fails. City civilians only share their position, not
  their health.

## Unreleased - full co-op spec, phase 3: 2-4 players

- **Up to 4 players** (`MaxPlayers`, default 4). The host forwards each player's data to
  the others and tells everyone who is in the session. A joining player loads the save
  alone. *Bot-tested* with two game copies and a headless third player; twin `MaxPlayers =
  2` refuses player 3.
- **Every player has a figure on every screen.** Players 3 and 4 spawn from C++
  (`ExtraPlayerFigures`, default 1). *Bot-tested*: host and client both see player 3's
  figure near the host and moving; twin with the switch off fails.
- **Per-player features:** the takeoff vote waits for every player and boards into the
  pilot's ship; quest rewards arrive once per player even when several players announce
  them; downed/revive per player (all-down game over, revive the downed player in range);
  dropped items appear at the dropper; kill XP and a client pilot's flight data work.
  *Takeoff vote bot-tested with 3 players* (the launch waits for player 3; twin where
  player 3 never answers: no takeoff). Rewards: unit tests. Downed/revive, drops, XP:
  not tested in game yet. Voice: still first partner only.
- **Takeoff test fixed.** The bot called `LaunchImmediate`, which takes off by itself, so
  the phase 1 test never tested the vote's hold (it passed by timing). The bot now opens the
  takeoff menu as a pilot does and fires the launch event the vote holds.
- **The F9 buddy works.** Its `PlaceAtMe` passed 5 of the 8 arguments Starfield needs and
  never spawned anything.

## Unreleased - full co-op spec, phase 2: direct sessions

- **Join by code, no relay or lobby server** (`DirectSession = 1`). The host's game
  publishes a join code (log, `PULSAR_JoinCode.txt` next to `PULSAR.ini`, clipboard,
  on-screen message) with its LAN address, its public address (asked from a STUN server
  over the game's own socket) and a port its router opened by UPnP. A friend sets
  `JoinCode`; their game tries every address at once. If none answers, it publishes a
  reply code; the host presses F2 with it on the clipboard (`HotkeyPasteReplyCode`) or sets
  `ReplyCode`, and punches back. Only holders of the code finish the encrypted handshake;
  an old or wrong code now says so and stops instead of a session where nothing arrives.
  *Bot-tested* (`-JoinByCode` PASS, `-StaleJoinCode` twin FAIL) on one PC through the LAN
  address; STUN, UPnP and a connection between two homes are not tested yet. Status of
  every spec item: [docs/SPEC-STATUS.md](docs/SPEC-STATUS.md).
- **Launcher: "Play with friends, no server".** Host gets a join code to copy; a friend
  pastes it and clicks Join; if a reply code is needed, it shows up on the friend's side and
  the host pastes it into "Send to game" (`PULSAR_ReplyCodeIn.txt`). The launcher checks a
  code for typos before starting the game (`Services/JoinCodeText.cs`, 8 C# tests pinned to
  the same sample code as the C++ test). A lobby or by-IP launch now switches a leftover
  join-code session in `PULSAR.ini` off. *Reply-code handover bot-tested* (this session's
  code accepted, another session's refused); the launcher screen itself not tried.

## Unreleased - phase 1 of the co-op spec

- **Doors move only the player who uses them.** A load door the host uses no
  longer pulls the client along; takeoff, landing and fast travel are unchanged.
  Setting: `CoopDoorFollow` (default `0`). *Bot-tested* (`door`).
- **Instanced enemy loot.** Each player loots their own copy of a dead enemy's
  items. Setting: `CoopInstancedEnemyLoot` (default `1`). *Bot-tested* (`loot`).
- **Quest rewards for everyone.** Items and credits a quest gives one player are
  given to the other too, once, unless their own copy of the quest already gave them.
  Setting: `CoopQuestRewards` (default `1`). *Bot-tested* (`reward`). Quest XP and
  weapon mods on reward weapons are not shared yet.
- **Takeoff ready vote.** When the pilot launches, the ship waits until every partner
  presses F3 (`HotkeyTakeoffReady`), moves a partner who is outside back aboard, then
  takes off. Unanswered for 60 s: cancelled. Settings: `CoopTakeoffVote` (default `1`),
  `CoopVoteMessages`. *Bot-tested* (`takeoff`, also with the partner outside the ship).
- **On-screen messages** for the vote, vehicles and the AI buddy, through one safe
  path (`UI/HudMessage`). Fixes a client crash when a message was shown. *Bot-tested*
  for the vote messages; the vehicle and buddy messages were not triggered by a bot.
- **Papyrus calls from C++ could crash the game.** The local CommonLibSF patch freed
  script call arguments with the wrong allocator. Fixed in `patches/commonlibsf.patch`.
  *Bot-tested* (`papyrus`).
- **Host game-thread time in co-op cut by about 75%** (from 260–395 to 70–93 ms per
  second). A check for new engine saves listed the saves folder several times per frame;
  it now runs 4 times per second. The key mirror and the gravity guard skip per-run work
  too. Found with a new dev-build section profiler (`TaskProfile` log line).
  *Bot-tested*: follow, loot and takeoff runs pass after the change (no twin: nothing
  is switched off).
- **F8 no longer fires a leftover takeoff-launch debug path** unless `DevHotkeys = 1`.
  F8 is the regroup key, and every press also fired the engine's takeoff launch event,
  past the takeoff vote's hold. A bot run that presses F8 mid-vote (`-F8DuringVote`)
  passed with and without the change: the engine ignored that event, so no bug was
  shown. A defensive change, not a demonstrated fix.
- **Docs:** [docs/COMPATIBILITY.md](docs/COMPATIBILITY.md) (versions, load order,
  other mods) and [docs/PERFORMANCE.md](docs/PERFORMANCE.md) (players, frame rate,
  bandwidth, tick rate, all measured). New log lines for both: `NetStats` and
  `TaskStats` in `PULSAR.log` every 10 s; the test harness logs the frame rate.
  *Stats tested* (`test_perf_stats.cpp`).

## Unreleased - gravity guard, bridge restore retry

- **Client gravity after a station or space transition (known 0.3.0 bug).**
  New `Coop/GravityGuard`: logs the local player's gravity state (`GRAV:`
  lines) around every transition and every `ActionGravityOn`/`Off` the engine
  runs on the local player. On the client, after a zero-G to gravity transition
  in which the engine did not run `ActionGravityOn` (AACT `0x00024655`, read
  from `Starfield.esm` 1.16.244), it fires it once. `HotkeyGravityFix` (F4)
  fires it on demand. Settings: `CoopGravityAutoFix`, `HotkeyGravityFix`.
  *Settings parsing tested; the gravity behaviour is not tested in game.* The
  cause is a theory: co-location teleports skip the load-door path.
- **Bridge restore retry.** If the player has no cell yet on the first tick
  after a bridge load, `InventorySync::DoApply` now retries for up to 30 s
  instead of dropping the restore (which left the client on the host's
  character). Setting: `CoopBridgeRestoreRetry`. *Settings parsing tested; not
  tested in game.*

## 0.7.4 - Toward 0.7.5: clone animations

New milestone 0.7.5: the partner's clone should play strafe and backwards
animations instead of an NPC forward run (the original author's "anim-graph
swap"). Findings and plan: docs/design/specs/2026-10-19-clone-animation-states.md.

- **Found in the game's animation data:** players and NPCs share one graph
  (`Human_Root.agx`). The difference is the state: the player runs in states
  0-8 (HumanDefault, HumanSneak, ...), NPCs in 10-19 - and PULSAR has always put
  the clone in the NPC states.
- **New experiment `CloneUsePlayerStates`** (off by default): the clone gets the
  host's own player state instead. *Not tested in game - docs/TESTING.md
  scenario J, with the fake peer.*
- **NPC movement direction fixed (original 0.3.0 bug).** The graph's
  `Direction` is a fraction of a turn (0 forward, 0.5 backwards), but NPC sync
  wrote radians into it, so sideways NPCs sent values outside the graph's range
  ("stares one way and jogs"). Now converted. *Conversion tested (4 tests); not
  tested in game.*
- **`DirProbe` log lines** (dev build): your own `Direction` next to your real
  movement angle, to check the above in game.

## 0.7.3 - Your own background traits work in your partner's world

- **The joining player can use their own background traits' NPCs.** Decompiled
  from the game: `TraitQuest` runs once per save and clears the quest alias of
  every background NPC the save's owner does not need - for example Andy Singh
  (House of the Enlightened chest) when the host is not "Raised Enlightened",
  Keeper Aquilus (Sanctum Universum chest) when the host is not "Raised
  Universal", plus Mirza (Serpent's Embrace), Madame Sauvage (Neon Street Rat),
  Mayor Cartwright (Freestar settler) and Administrator Rookes (UC native). The
  joining player is in the host's world, so their own trait NPC was missing:
  an Enlightened friend could not get their chest next to a Universal host.
  Now, once the joining player's own traits are restored, PULSAR re-fills the
  alias their traits need when that NPC is nearby (`CoopTraitNpcsForPartner`,
  on by default). The host's own setup is untouched.
  - Each player keeps only their own traits after joining (that part already
    worked: the restore replaces the host's perks with the client's).
  - Dialogue sync only mirrors the lines; each player talks with their own
    traits, so trait-gated lines show for whoever has the trait.
  - *Rules tested (4 tests, a copy of the quest script's logic). In game: not
    tested yet - see docs/TESTING.md scenario I.*
- **About 1 in 256 network packets was silently thrown away (original 0.3.0
  bug).** Encrypted packets start with a random 12-byte nonce, but the receiver
  treated any packet whose first byte was 0xA1 as the key-exchange hello and
  dropped it - including reliable packets ENet had already acknowledged, so they
  were never resent: a loot pickup, a quest stage, a door, even the save
  transfer could vanish. Found through a flaky fake-peer test (15% of runs).
  The hello is now only accepted before the session key exists. *Regression
  tested (an encrypted packet that starts with 0xA1 is decrypted, not dropped);
  the fake-peer suite went from 6/40 failing runs to 0/40.*

## 0.7.2 - Your partner counts as a human follower

- **Extrovert / Introvert now notice your co-op partner.** Read from
  Starfield.esm: both traits switch on `TRAIT_CND_HasHumanFollower`, which is
  "PlayerFollowers_CountFollowing >= 1, and >= 2 if Vasco is active". The
  partner's clone is not a recruited follower, so before this Extrovert never
  counted your friend and Introvert treated you as alone next to them. While
  the partner is in the same area and within 40 m, PULSAR now raises that count
  (`CoopPartnerCountsAsFollower`, on by default).
  - The follower script (SQ_FollowersScript) owns the count and rewrites it with
    whole numbers at any time, so PULSAR adds 1.5 instead of 1: a value ending
    in .5 is always PULSAR's, so it never removes a real follower from the count,
    and a leftover caught in a save is removed later. No other condition or
    script in Starfield.esm reads this global.
  - *Logic tested (7 tests). In game: not tested yet - see docs/TESTING.md.*

## 0.7.1 - Distances are metres

First in-game check (one PC, with the fake peer): the version check, encrypted
connection, the 90 s reconnect rule and the clone spawn all worked. The clone was
placed ~350 m away, which showed that **Starfield positions are in metres**, not
the ~1.4 cm "game units" some code comments assume.

- **Correction to 0.5.0:** the "passenger carry could never board" units fix was
  wrong. The original 4 / 1 / 0.6 m values were right and are restored. Same for
  the F9 buddy's follow distances (6 m / 40 m), which I had scaled up ~70x.
- **Vehicle guard:** "the host's vehicle" is now within 20 m (was 1500).
- **Fake peer:** stands 2 m from the host and circles at 3 m by default (was 200
  and 300, i.e. hundreds of metres away); walk speed thresholds are in m/s.
- **Revive range (original 0.3.0 code):** the revive key reached a downed partner
  up to 250 m away (`kReviveRadius = 250`, commented "~3 m"). Now 3 m.
  *Not tested in game.*
- Other original constants with the same "game units" comment, to check in game:
  cell-content probe match radius 500 ("~7 m"), dialogue "same scene" distance
  6000 ("~60 m"), NPC door radius 3000 ("~30 m"). Left unchanged: they are
  probably far too large, but changing them without play could break matching.

## 0.7.0 - Release preparation

This is the end of the unattended 0.3.1 -> 0.7.0 run. **None of 0.3.1-0.7.0 has
been played yet.** The roadmap goal for 0.7.0 ("a two-player playthrough without
major workarounds") can only be met by playing; see docs/TESTING.md for the
checklist and README "Where we are" for what to verify first.

- **The shipped DLL passes the leak check again.** The hardened Release build
  refused the new tester-visible log lines (version refused, quest stages
  replayed, automatic regroup, vehicle refused). They were added to the
  allow-list on purpose; the build now reports 35 curated lines and no leaks.
- **Packaging works in this repository.** `tools/package_release.ps1` looked for
  the compiled scripts and the .ba2 archive in `scripts\` (they are in `data\`),
  required the VS 2026 build folder and `cmake` on PATH, and needed `-EspPath`.
  It now defaults to `data\PULSAR.esp`, finds `build\vs` or `build\vs2022` and
  the VS-bundled CMake, and ships LICENSE-ADDITIONAL and this changelog.
- **Named and credited as a modified version**, as LICENSE-ADDITIONAL requires:
  packages are `PULSAR-Revived-*.zip`, the tool's window and About page say
  "PULSAR Revived", "Modified version. Based on PULSAR by ramisotti13-eng", with
  links to the original and to this fork.
- **Tool update checks point at this fork** (`Pinganiyo/pulsar-tool`) instead of
  the original, unmaintained repositories.
- **Continuous integration:** `.github/workflows/build.yml` builds the plugin,
  the tests and the tool on every push and pull request and runs
  `pulsar_tests.exe` and the companion tests. *Not yet run: it starts on the
  next push to GitHub.*

## 0.6.0 - Completeness

- **Equipment sync is not "disabled", and weapons are now a setting.** The 0.3.0
  notes say equipment sync is off; in the code, armor has synced onto the
  partner's clone since June 2026, and weapons were switched back on in a later
  experiment although weapon equips had been linked to a clone animation crash.
  New `SyncCloneWeapons` setting (default on, same as before): set it to 0 if
  the game crashes when your partner draws or swaps weapons. *Not tested in game.*
- **PULSAR.ini parsing is tested.** Every setting testers are told to change goes
  through it; 5 tests cover defaults, the new settings, key spellings, comments
  and bad values. *Tested.*
- Reviewed for long sessions: corpse-loot packets (sender caps at 24 items,
  receiver checks every length) and growing per-NPC caches (bounded in practice,
  a few MB at most). No change needed.

Still open in this milestone, needing play sessions: whether weapon sync is
stable, corpse loot sync in play, host-authoritative NPC firing (not built),
and long sessions with progression across saves.

## 0.5.0 - Gameplay

- **Riding the host's vehicle no longer crashes the client - it is refused.**
  Formally entering the vehicle the host is driving crashes the client. New
  `SharedVehicleRide` setting in PULSAR.ini: `off` (default) refuses that entry
  with an on-screen message; `carry` also refuses it but carries you as a
  passenger when you walk up to the vehicle (experimental); `enter` keeps the
  old behaviour. *Rules tested (6 tests); not tested in game.*
- **Passenger carry could never board.** Its distances were written as metres
  but compared with positions in game units (~1.4 cm), so you had to stand
  within ~6 cm of the vehicle. Fixed. *Not tested in game.*
- **Quest stages missed during a short disconnect are replayed.** A client that
  reconnects within 90 s does not get a fresh save, and quest stages the host
  set meanwhile were never sent, so the client's quests fell behind. The host
  now replays them; the client skips stages it reached itself, so stage scripts
  do not run twice. *Journal tested (4 tests); not tested in game.*

Still open in this milestone, needing play sessions: verifying combat, deaths,
revives, loot, inventory persistence, doors, containers, NPCs and cell
transitions with two players.

## 0.4.0 - Core fixes

- **F8 now works for the client.** It used to do nothing on the client (the
  handler returned for anyone but the host), although the README tells the
  client to press it. It now asks the host to regroup the client. This is the
  recovery for the space station / POI divergence and "stuck inside the ship".
  *Not tested in game.*
- **Optional automatic regroup in space** (`AutoRegroupInSpace = 1` in
  PULSAR.ini, off by default). If the client is in space and its cell disagrees
  with the host's for 20 s, it asks the host to regroup. Max 3 per session, 2 min
  apart. Off by default because a false trigger reloads the save. *Trigger rules
  tested (7 tests); not tested in game.*
- **Auto-reconnect could stop for good.** If creating the network host failed
  during a reconnect, no retry was ever scheduled. It now keeps the old host and
  retries in 5 s. *Not tested in game.*
- **Character gains merge is now tested.** The rule that decides which items a
  client keeps from a session moved to its own file, unchanged, with 8 tests
  (gains, losses, host items excluded, restore leftovers cancel, failed re-adds
  kept). *Tested.*
- Profile restore after a regroup and the space-flight physics fix from 0.3.1
  are part of this milestone too. *Not tested in game.*

Still open in this milestone: host gear left as an unusable "ghost" on the
client (the remaining cause is the game's inventory UI cache and needs in-game
reverse engineering), and confirming the gravity and POI fixes in game.

## 0.3.1 - Stabilize

- **Version check on connect.** The client used to send a hard-coded 0.1.0 and
  the host never checked it, so players on different builds could connect and
  desync or crash. The client now sends its real version; the host refuses a
  different one, logs who runs what, and the refused client logs which version
  to install and stops auto-reconnecting. Peers on 0.3.0 or older are refused
  as "0.3.0 or older". *Rules and reason codes tested; not tested in game.*
- **Handshake player name is bounded.** It was logged without making sure it was
  null-terminated. *Tested.*
- **Fresh clones build.** The CommonLibSF submodule commit is now recorded;
  Visual Studio 2022 builds documented.
- **Automated tests** (`pulsar_tests.exe`): profile codec, encryption, relay
  protocol, packet table, loopback networking, fake peer, version rules and
  release bookkeeping.
- **Fixed by the tests:** names with quotes broke the saved character profile;
  a truncated profile was accepted as valid (items could be lost); any 28-byte
  packet was accepted as an authentic encrypted message. *Tested.*
- **Profile restore after a mid-session regroup** no longer skips the restore
  when the local profile file is missing. *Not tested in game.*
- **Space flight sync** drops its cached physics-body pointers on every save
  load instead of writing through freed memory (candidate for the stuck-gravity
  report). *Not tested in game.*
- **Tool:** finds the game and the mod files in more places; installs a dev
  build's `PULSAR.dll`; can install before SFSE is present.
- **One-PC testing:** F9 buddy (AI teammate) and `pulsar_fakepeer.exe` (a
  headless second player). See docs/TESTING.md. *Not tested in game.*
- **Release bookkeeping:** `tools/bump_version.ps1` sets the version in all four
  places; a test fails if they drift apart or this file has no entry.
