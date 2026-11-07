# Flying together: where the ship's position lives (1.16.244)

**Date:** 2026-10-31 to 2026-11-07
**Status:** the passenger's view follows the pilot's (fix in `FlightSync.cpp`, see "Fix"
below); time sync, steering checks and a bot twin still open. The probe used to find all
this (write watches, live request files, the time warp, the override hook in
`src/SpaceSync/SpaceFlightProbe.cpp`) stayed in the working tree and is not committed:
it hooks engine functions and sets hardware breakpoints, test copies only.

## Problem

The owner's report: the pilot (host) sees what is in front of the ship, the passenger
(client) does not. The client's ship copy turns with the host's (V3 rotation sync, works),
but it does not move with it. `kShipPosSyncEnabled = false` in `SpaceSync.cpp` because the
ship REFR stays at (0,0,0) while flying.

## Findings

All RVAs are Starfield.exe 1.16.244 (image base 0x140000000).

1. **Things in space are drawn relative to an origin.** The Eye (REFR 0x00012894, cell
   0x0001285A, `data.location` = 0,0,0) gets its node's local translate written every
   frame on the game thread by `+0x23486C0` (copy into node+0x70), called from
   `+0x23485D0`:
   `drawn = (object doubles at obj+0x30..0x48 - origin) * scale` (float result).
   Its caller `+0x23447E6` passes origin = `r12+0x110` (4 doubles) and scale = `r12+0xD8`
   (1.0). The origin owner (`r12`) is a heap object whose first field is a scene node.
2. **The origin is the ship's position in the star system, in metres** (about
   (-2.066e11, 8.41e10, 1.98e10) near The Eye). It moves about 18 km/s: the planet's
   orbital motion. The station on screen moves only about 4 m/s (the ship's own drift),
   so what has to match between the two games is the ship's position relative to the
   planet/station, not the absolute value.
3. **The origin is copied each frame** (`+0x2344379..0x2344395`) from an entity record
   found by an ECS query (`+0x23FFB10`): record +0x10..0x3F = 3x4 float rotation, +0x40..0x58
   = position doubles, +0x60..0x78 = previous position. Writing the record (+1000 m) has no
   effect: it is rewritten next frame.
4. **The record is written by a job** (`+0x9522D0`, write at `+0x9528CC..0x9528DB`, worker
   threads): `record.pos = parent.pos (doubles at parent+0x40) + scale * float offset
   (record+0x80)`; the offset is 0 for the ship, so the ship's position is its parent's.
5. **The parent's position is written by `+0x2348340`** (called from `+0x2348A30`, worker
   threads): `pos = this+0x20..0x38`, plus a per-frame delta from `[this+0x48]+0x60..0x78`
   (then reset), stored to `[this+0x48]+0x40..0x58` and back into `this+0x20`. Writing
   `this+0x20` (+1000 m, from outside the process) also had no visible effect: it is
   recomputed upstream as well, probably from the orbit (planet position + the ship's
   offset from it).
6. **Host and client really are in different places** relative to the station: the host
   drew The Eye 754 m away, the client 859 m away in another direction, both drifting
   ~5 m/s from the speed stored in the save, in opposite directions.

Tools used: a hardware write watch (debug registers on every thread, vectored handler,
unwound callers), live requests through `PulsarProbeWatchAddr.txt` / `PulsarProbeRead.txt`
in the test copy's plugin folder, and a disassembler script (bytes compiled into a code
section of an object file, then `dumpbin /disasm`; `dumpbin /range` on the exe decoded
the wrong place for `+0x9522D0`).

## Update 2026-11-07: the position comes from an orbit

7. **Three writes per frame to the ship object's position** (`this+0x20`, watched on the
   client): `+0x234A783` (copy from the orbit component `[this+0x40]+0x80..0x98`),
   `+0x23472CC` (the orbit itself) and `+0x23484B4` (the store back seen in 5). All run in
   the per-object space update `+0x234A5D0` (rcx = space object, xmm1 = dt), called from
   the job `+0x234BC80`.
8. **The ship flies on a Kepler orbit.** `+0x23471E0` computes the position from orbital
   elements on the orbit component (doubles at +0xA0, +0xA8, +0xB0, +0xB8; Kepler solve,
   sin/cos, the rotation matrix at +0x40, plus +0x60..0x78 and the parent body's position).
9. **The engine has a position override:** at the start of `+0x234A5D0`, if byte
   `[this+0x48]+0x80` = 1 it copies `[this+0x48]+0x40..0x58` into the position (+0x81 = 1:
   adds it), clears the bytes and refits. Tested on the client:
   - written from outside the process in a 0.5 s loop: the position changed, then the game
     froze within a second and exited without a dump (the write lands mid-update);
   - set from a MinHook on `+0x234A5D0` for the player's ship only, once, right before the
     original runs: no freeze, but no effect either: station minus ship stayed
     (92.3, 269.5, -96.8) at 100 ms sampling, so the orbit result overwrites it in the same
     update.
10. The probe now finds the ship's space object by itself (four automatic watch rounds,
    `SHIP OBJECT` in the log, about 13 s after the first space frame); tools for reading
    memory and disassembling are in `instances/re-tools/` (outside git).

## Update 2026-11-07 (2): the client's universe runs behind; a time warp fixes it

11. **Orbit component layout** (`[ship object+0x40]`, host and client side by side):
    +0x00..0x3F 4x4 float rotation, +0x48/+0x50 doubles (small value, angle about -2.0),
    +0x80..0x98 absolute position, +0xA0 double ~13650, +0xA8 = 1.0, +0xB0 = 0, +0xB8
    ~0.000444, +0xC4/+0xC8 floats (phase, advanced each frame in the function's tail),
    +0xCC mode byte, +0xCE "skip orbit" byte (0 = orbit on). Host and client values differ
    slightly; the absolute positions differ by ~2900 km.
12. **The client's whole universe runs 21-22.5 s behind the host's**: its origin (ship
    position in the system) is where the host's was 21-22.5 s earlier, to within 0.2 s.
    It is the save-load time: orbits are advanced by each frame's dt, not computed from a
    clock, so the host's universe moved on while the client loaded. `GameDaysPassed` is
    already in step (within 0.3 s) and is not what drives the orbits.
13. **A time warp catches it up.** Hooking `+0x234A5D0` and adding seconds to dt once for
    every space object (25 objects, within 150 ms) moves planets, stations and the ship
    together. The effect is 0.2 x the added seconds (1/TimeScale, TimeScale = 5): 105.8 s
    took the lag from 21.16 s to 0.56 s, reproducibly. No freeze.
14. **Still different after that: the ship relative to the station.** Host: station minus
    ship (-610, 204, 402); client after the warp: (1058, 2363, -582) (the warp itself moved
    the client's ship ~2 km relative to the station). Copying the host's orbit component
    bytes (+0x00..0x7F, +0xA0..0xCB) into the client, from the hook, changed nothing. The
    ship's position relative to the station is decided elsewhere (parent chain, or the
    ship's own state) and is the next thing to find.

15. **The orbit is refit from the ship's position**, so copying it cannot work:
    `+0x2347320` (writes the elements, e.g. +0xA0) is called from `+0x23488C0`, which takes
    the ship position (rdx) and the centre (r8), computes the distance and two angles
    (atan2 at `+0x34CA906`) and fits a circular orbit at that distance. The host's own
    elements change continuously (+0x48, +0x50, +0xA0, +0xC4 all moved within minutes).
16. **The override is consumed, yet station minus ship does not jump.** Set through the
    hook (-500 m on z, sampled every 100 ms around it): no step in station minus ship, and
    the flag bytes `[this+0x48]+0x80/0x81` read 0 afterwards. `+0x234A5D0` begins by
    copying r8 (4 doubles) to `this+0x00`, dt to `this+0xA0`, then calls `+0x232D340` only
    when `this+0x48` is empty (not the case for the ship), then the flag check. Open: why a
    consumed override leaves no trace in the ship's position relative to The Eye (another
    writer later in the update, or the measurement points at a copy).

17. **Nearby objects orbit the player ship.** The Eye's space object (same type as the
    ship's, allocated next to it) has the ship's space object as parent (+0x78); the ship's
    own parent is another object (likely the planet). The Eye's record (`[obj+0x60]`) holds
    its absolute position at +0x30 (written by `+0x2348496`) and its drawn position at
    +0x60 (written by `+0x234865A`, a by-product of drawing). So what a passenger sees out
    of the window is The Eye's position relative to the ship, and moving the ship cannot
    change it: a traced ship shift took effect (the next update started from the new
    position) and The Eye moved with it.
18. **Setting The Eye's position relative to the ship through the override did nothing**,
    not even inside the update it was set in (traced over 8 updates). In that run the
    client's Eye sat still at (92.0, 269.5, -96.7) from the ship before every update, while
    the host's drifted ~5 m/s; in other runs the client's drifted too, sometimes opposite.
    The difference is the ship's velocity, which seems to live in the physics (Havok), not
    in these records.

## Update 2026-11-07 (3): it is the ship's physics body - and V3 already carries it

19. **The ship moves through its Havok body.** The movement delta (ship record +0x60) is
    written by `+0x232E178..0x232E196` (in `+0x232DF50`, called from the space update):
    `delta = body position (floats, body vt[0x248]) + global double origin
    (Starfield.exe+0x5F60520) + global float shift (+0x5F60200) - record position`.
    (Corrected: the disassembler script prints RIP-relative targets relative to the
    snippet start, so the function start 0x232DF50 has to be added; an earlier version of
    this note gave +0x3C325D0/+0x3C322B0.)
    Body = `[[ship space object+0x68]+0x60] + 0x20` (vtable Starfield+0x4D65D60).
20. **Body data block `[body+0x4D0]`**: +0x50..0x7F rotation (3x4 floats), **+0x80 position**
    (floats, physics-world frame), +0x90/+0xA0 bounds, **+0xB0 linear velocity** (get/set
    pair vt[0x270]/vt[0x278]). Host: position (2804, -427, -1826), velocity (4.21, -0.99,
    -2.67) = the 5.08 m/s drift (a 4 % throttle from the save). Client: position (0.4, -0.1,
    -0.3), velocity 0 - its ship has no pilot, and a written velocity was braked to 0
    within a second.
21. **Writing the host's body position into the client's body puts the station where the
    host sees it**: station minus ship (-2928, 747, 1867) on the client against
    (-2943, 751, 1876) on the host (the host drifted on in between). No time warp needed
    for this: nearby objects are placed relative to the ship's body.
22. **PULSAR's V3 sync already sends this block** (`havokState` = the 64 bytes at
    `[body+0x4D0]+0x50`, position included) and the client already applies it
    (`SafeCommitHavokTransform`, tick body apply). It never happens because the host
    never identifies its own ship body: `M.1 V2` matches bodies against
    `ship->data.location` when nothing is received, which is (0,0,0) while the host's body
    sits 3.4 km away; no "player-ship body acquired" on the host, so it sends zeros and the
    client skips them (all-zero check). The old note "translation stays 0 (cockpit
    anchor)" came from reading the ship's scene node, not the body.

## Fix (2026-11-07): find the own ship body by the draw origin

23. **The player's own ship body sits exactly at the draw origin.** The draw origin is
    `[[Starfield+0x5FB08E8] + 0x90] + 0x110` (3 doubles; the caller `+0x18849BE` loads the
    owner from that global). Body position + G (+0x5F60520) + F (+0x5F60200) - origin =
    (0.0, 0.0, 0.0) on the host. `FlightSync::SafeReadOwnShipBodyRef` returns
    origin - G - F, and the M.1 V2 body match uses it as its reference on both sides
    (config `CoopShipBodyByOrigin`, default 1; 0 = the old reference).
24. **Result** (run 20261107-160008): the host logs `V3 iter8 active read` with its body
    position moving, the client logs `V3 iter13 tick-body-apply` with the same values
    0-62 ms old, and The Eye relative to the ship matched on both sides one second apart
    (host (-523.0, 184.0, 349.1) at 16:03:58.5, client (-526.9, 185.0, 351.7) at
    16:03:59.5), drifting the same way. Watched by the owner from the client's passenger
    seat while flying the host toward the station: looks right.

## Next steps

- ~~Steering and thrust~~: checked 2026-11-07 (run at 17:00, owner flying the host:
  turns, up to 98 m/s, stop; `instances/re-tools/steer.ps1` sampling both ships' body
  every ~0.5 s). Heading identical in every sample (0.00 deg; the 64-byte state carries
  the rotation). Position: median 0.3 m apart, 4.0 m at speed, at most 14.6 m at 98 m/s
  (about 0.15 s of travel: send interval plus the client's body standing still between
  updates).
- **Open: the passenger's view is unreliable.** The bot `fly` scenario compares where
  The Eye is drawn relative to the ship on both sides (node local translate). Runs
  2026-11-07: with the clock catch-up on, 1 of 5 passed (195931); with it off, 2 of 3
  (201421, 201858 passed; 202939 failed at 1658 m). The twin with the old body search
  fails at 2058 m (202500). In a failing run the client's view of The Eye swings about
  1.5 km to and fro with a period of roughly a minute while the host's drifts evenly at
  ~4 m/s: something on the client fights the synced ship state (its own flight
  physics, the ship's orbit refit, the velocity or prediction write?). Ruled out: the
  clock catch-up alone, and the near objects being parented to the ship (their parent
  was the same 2,560 km body in passing and failing runs). One failing run's client
  re-acquired its ship body with d2 = 3.4e38 (no real match, a race in the M.1 V2 pick);
  another failed with a real match, so that is not the whole story.
- ~~Set the client body's velocity from the host's~~: built (`CoopShipVelocitySync`,
  `ShipTransformData.havokVelocity`), plus moving each applied position on by velocity x
  the update's age. Three owner flights sampled with `steer.ps1` (the last one comparing
  the client with the midpoint of two host reads): gap in seconds of travel, median /
  95 % / max: position only 0.067 / 0.154 / 0.154 s (98 m/s); + velocity 0.084 / 0.162 /
  0.170 s (428 m/s); + prediction 0.068 / 0.151 / 0.209 s (427 m/s). No measurable gain:
  the rest is a few frames (each side steps physics and hands on its state once per
  frame). The owner found the view smoother with it, so it stays on.
- The client's body is braked to speed 0 between updates (no pilot); if the movement looks
  stepped, also set its velocity (`[body+0x4D0]+0xB0`, vt[0x278]) from the host's.
- ~~The client's universe runs ~21 s behind after loading~~: built as `SpaceClockSync`
  (`CoopSpaceTimeSync`). The host sends its draw origin (`PKT_SPACE_CLOCK`, 2 Hz); the
  client extrapolates it with the host's velocity, takes the lag along that velocity
  (ignored if more than 20 km off the host's track), and adds lag x TimeScale x 0.96 to
  every space object's dt once (hook on `+0x234A5D0`, 150 ms window, each object once).
  Run 20261107-163044: 20.28 s -> 0.09 s in three catch-ups (one window caught no update
  and was retried), then a slow fall-back of ~0.01 s/s topped up at 0.5 s; twin
  20261107-163620 stays 22.0-22.5 s behind.
- A bot check with a twin (`CoopShipBodyByOrigin = 0`).

- Most promising: sync the orbit, not the position. Dump the host's and the client's
  orbit components (`[ship object+0x40]`, about 0x100 bytes) side by side, find which
  fields are the elements and the epoch, then copy the host's elements to the client
  (from the hook on `+0x234A5D0`, same thread). Thrust refits the orbit, so the host sends
  the elements whenever they change.
- Check that both games have the same orbital time (planet positions), or sync it.
- Then: host sends relative position + velocity at ~20 Hz; client writes them where the
  engine reads them, with the V3 rotation sync kept as is.
