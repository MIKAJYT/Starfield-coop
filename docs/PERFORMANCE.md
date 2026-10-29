# Players, frame rate, bandwidth and tick rate

The spec asks for a target frame rate, a maximum player count, and a rough bandwidth
and tick rate. This page gives the targets and what was actually measured. Each number
says whether it was **measured** (with the run it came from), **from the code**, or
**derived**.

## Players

**2.** The network layer accepts exactly one peer (`MAX_CLIENTS = 1` in
`include/Network/NetworkHost.h`), and every sync module assumes one partner. 3–4
players would mean reworking that layer and each module; that work hasn't started.
[OVERVIEW.md](OVERVIEW.md#structural-limits) explains why a large server isn't
possible.

## Frame rate

### Targets

These are proposed targets, not measurements; the owner can change them.

- **No frame rate lost to co-op** that a player can notice: within 10% of the same PC
  playing solo at the same settings.
- **30 fps or more** for both players on a PC that runs Starfield at 30 fps or more solo.
- PULSAR's own game-thread time stays **under 2 ms per frame**.

### Measured

Setup for every number below: **both games on one PC** (a gaming laptop with an
8-core Ryzen 7 and an RTX 4060 Laptop GPU), 1920×1080 output, render scale 0.5,
the lowest quality preset and the memory-saving settings the test copies use. With
two copies sharing one GPU, these numbers are **far below** what one player on one PC
gets, and are only good for comparisons within the same run.

| Situation | Host fps | Client fps | Run |
|---|---|---|---|
| One copy running alone, Akila City | 85 | – | 20261029-175650 (before the client started) |
| Both copies running, **not connected**, Akila City | 38–42 | 47–50 | 20261029-180151 |
| Both copies running, **co-op**, Akila City | 36–40 | 47–50 | 20261029-175650 |
| Co-op, aboard the landed ship, then in space | 50–52 | 49–55 | 20261029-180739 |

Each range covers the steady 10-second windows (load screens left out).

**Result:** connecting didn't lower either player's frame rate measurably on this PC;
the second copy of the game costs the frames (85 → about 40).

**PULSAR's own game-thread cost** (time spent in its per-frame work, measured by the
plugin itself):

| Situation | Host | Client |
|---|---|---|
| Solo (not connected) | 29–50 ms per second | 24–57 ms per second |
| Co-op, Akila City | **260–355 ms per second** (about 9 ms per frame at 38 fps) | 66–104 ms per second |
| Co-op, ship and space | 320–395 ms per second | 66–82 ms per second |

**The host's co-op cost misses the 2 ms per frame target by far.** It didn't cost
frame rate here, which suggests the GPU (shared by two games) was the limit, so the
game thread had time to spare. **Derived, not measured:** on a
PC where the CPU (game thread) is the limit, the host could lose up to about a third of
its frame rate. What uses the time is not yet profiled; NPC sync is the first suspect,
because it is the largest stream the host sends (below).

Not measured: a real two-PC session, one game per PC; frame rate in combat; long
sessions.

## Bandwidth

Measured over the network on one PC (loopback, no internet latency), from ENet's own
counters: everything on the wire except the UDP/IP headers, which add 28 bytes per
datagram (included in the "with UDP/IP" column). 1 KB = 1024 bytes.

| Situation | Host → client | Client → host | With UDP/IP | Run |
|---|---|---|---|---|
| On foot in a busy city (Akila City) | 137–140 KB/s (164–173 datagrams/s) | 10–11 KB/s (83–91 datagrams/s) | about 143 / 13 KB/s (≈ 1.2 / 0.1 Mbit/s) | 20261029-175650 |
| Aboard a landed ship | 19–23 KB/s | 9.6 KB/s | about 21 / 11 KB/s | 20261029-180739 |
| In space | 40 KB/s (67–74 datagrams/s) | 9.4 KB/s | about 42 / 11 KB/s | 20261029-180739 |
| Save transfer (join, takeoff) | one 5.4 MB packet; about 500 KB/s, so about 11 s | – | – | both runs |

Where the host's bytes go (application payload, before encryption):

| Stream | Akila City | In space |
|---|---|---|
| NPC state (`PKT_NPC_STATE`) | **119 KB/s**, 85 packets/s | 12 KB/s, 21 packets/s |
| NPC ships (`PKT_NPC_SHIP_BATCH`) | – | 7.9 KB/s, 9 packets/s |
| Player position (`PKT_POSITION`) | 4.4 KB/s, 40 packets/s | 4.4 KB/s, 40 packets/s |
| Ship transform (`PKT_SHIP_TRANSFORM`) | – | 4.3 KB/s, 37 packets/s |
| Skeleton (`PKT_BONES`) | 2.9 KB/s, 9 packets/s | 2.8 KB/s, 9 packets/s |
| Civilian snapshot (`PKT_CIVILIAN_SNAPSHOT`) | 0.9 KB/s, 2 packets/s | – |

The client sends mainly its position (4.4 KB/s) and skeleton (2.8 KB/s).

**Voice chat** (derived from the code, not measured: no microphone in the bot runs):
Opus at 24 kbit/s in 20 ms frames, sent only while talking (push-to-talk, or open mic
with a silence gate). With the packet, encryption and ENet overhead that is about 5 KB/s,
about 6.5 KB/s with UDP/IP, each way while someone talks.

**Rough rule:** plan for **1.5 Mbit/s upload on the host** and 0.2 Mbit/s on the client
(plus voice). Most home connections have that. Not measured: a real internet link, where the
save transfer will be no faster than above and may be slower with high latency.

## Tick rate

| What | Rate | Source |
|---|---|---|
| Network thread: send and receive | every 16 ms (about 60 per second) | code: `NetworkLoop` in `src/Main.cpp` |
| Player position, each way | once per frame, at most 60 per second; **measured 40/s** at 38–50 fps | code: `src/Main.cpp`; measured above |
| NPC state, host → client | each NPC at most 30 per second (33 ms), batched | code: `src/NPCSync/NPCSync.cpp`; measured 85 packets/s in Akila City |
| Civilian positions, host → client | 2 per second | code (`kCivIntervalMs = 500`); measured 2/s |
| Skeleton (bones) | 10 per second | code (100 ms); measured 9/s |
| Ship position in space | 30 per second | code (`kShipPosSendIntervalMs = 33`); ship transform measured 37/s |
| Voice | 50 frames per second while talking | code (`lobby/companion/Voice/OpusCodec.cs`) |
| Events (doors, loot, quests, takeoff vote, ...) | when they happen | – |

## How to measure it yourself

Every co-op session logs, in `PULSAR.log` (release builds too):

- `NetStats 10s: wire up … down …; payload …; top …` every 10 s while connected: the
  bandwidth and the six busiest packet types by bytes (numbers are the packet type IDs
  from `include/Network/PacketTypes.h`).
- `TaskStats 10s (solo|co-op): … runs/s, avg … ms, max … ms, game-thread time … ms per
  second` every 10 s: PULSAR's game-thread cost. "runs/s" is not a frame rate: SFSE runs
  plugin tasks each time an engine function runs, several times per frame.

The frame rate comes from the test harness (test copies only): `Frames 10s: … fps
(frame p50 … ms, p95 … ms, max … ms …)` in the harness log, counted at
`IDXGISwapChain::Present` on the game's swap chain. The shipped plugin has no render hook.
