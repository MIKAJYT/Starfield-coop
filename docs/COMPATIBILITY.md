# Versions, load order and other mods

What PULSAR Revived needs, where it goes in the load order, and what is known about
other mods. Each list says how it is known: **checked** (looked at or tested, with
the source), **derived** (follows from something checked, not tested itself) or
**unknown**.

## Versions

Both players need exactly these. There are no alternatives: PULSAR finds engine
functions by address, and the addresses change with every game update.

| Part | Version | How to check |
|---|---|---|
| Starfield | **1.16.244.0**, Steam (app 1716740, build id 23518663) | Right-click `Starfield.exe` → Properties → Details → File version. SHA-256 of the exe tested here: `7E9ADB1414A8E1B325E5E1F097B9B17B78DEB7EEBEDA37A333351A43A60F9D28`. |
| SFSE | **0.2.21** (`sfse_1_16_244.dll`) | [sfse.silverlock.org](https://sfse.silverlock.org/). The file name names the game version it is for. |
| Address Library for SFSE | **All in one, v22** (`Data\SFSE\Plugins\versionlib-1-16-244-0.bin`) | The file name must say `1-16-244-0`. |
| PULSAR Revived | the same build on both PCs | The host refuses a different version and says which one it runs (`src/Network/NetworkHost.cpp`, version check on connect). |

Checked:
- The plugin declares 1.16.244.0 as its only compatible runtime, uses the Address
  Library, and is layout dependent (`SFSE_PLUGIN_VERSION` in `src/Main.cpp`). SFSE
  refuses to load it on any other game version.
- Every automated co-op run in [TESTING.md](TESTING.md) used the versions above.

Not supported: the Game Pass / Microsoft Store version (SFSE runs only on Steam),
consoles, and any game version other than 1.16.244.

## Install and load order

PULSAR is three kinds of file:

| File | Where | What it is |
|---|---|---|
| `PULSAR.dll`, `PULSAR.ini` | `Data\SFSE\Plugins\` | The SFSE plugin: all the co-op code. |
| `PULSAR.esp`, `PULSAR - Main.ba2` | `Data\` | A small master plugin: one quest and 13 global variables the Papyrus side uses, plus one static object. |
| `PulsarBridge.pex`, `PulsarFurnSync.pex`, `PulsarRemoteControl.pex` | `Data\Scripts\` (loose) | Papyrus scripts the plugin calls. |

**Load order: put `PULSAR.esp` anywhere after `Starfield.esm`.** Its position among
other mods does not matter. Checked by reading the plugin's header and records
(2026-10-29):

- Its only master is `Starfield.esm`.
- It is flagged as a master (ESM flag) but is not a light plugin, so it takes one
  full plugin slot.
- It has 15 records, all new (13 GLOB, 1 QUST, 1 STAT). **It overrides no record
  of `Starfield.esm`**, so no other plugin can conflict with it record for record,
  and it changes nothing other mods change.

`Plugins.txt` (in `%LOCALAPPDATA%\Starfield`) needs the line `*PULSAR.esp`. The test
copies use exactly this `Plugins.txt`:

```
*PULSAR.esp
```

### Game settings

The loose scripts only load if the game prefers loose files over archives. These
`StarfieldCustom.ini` lines (in `%USERPROFILE%\Documents\My Games\Starfield`) do that;
the test copies use the same. Unverified: whether the original PULSAR tool's "Fix
all" sets exactly these.

```ini
[Archive]
bInvalidateOlderFiles=1
sResourceDataDirsFinal=

[General]
bPauseOnLostFocus=0
bAlwaysActive=1
```

`bPauseOnLostFocus=0` and `bAlwaysActive=1` keep the game running while its window
is in the background. Without them a player who alt-tabs freezes their side of the
session.

## Other mods

### Both players need the same mods, in the same order

**Derived, not tested:** the joining player loads the host's save, and co-op packets
name many world objects by their runtime form ID. A form ID from a mod includes that
mod's position in the load order. So:

- A mod the host has and the joiner doesn't: the joiner's game loads the save
  without that mod's content, and every packet about it points at nothing.
- The same mods in a different order: form IDs from those mods point at different
  objects on each side.

Item and NPC base forms are sent as plugin name plus local ID (loot, rewards, NPC
spawns, equipment), so those survive a different order. References, doors, quests
and most other state are sent as raw form IDs and don't.

### Mods that add or change records

Not tested with any. Derived from the above: they work in co-op only if both
players have them in the same order, and only as far as PULSAR syncs the things
they change. A mod that adds a new kind of interaction (its own menus, its own
scripts that move the player) won't be synced.

### Other SFSE plugins

Not tested with any. Things to know:

- PULSAR installs about 70 engine hooks (MinHook detours and a few call/vtable
  patches) in player movement, actor updates, inventory and container changes,
  activation, doors, quests, dialogue, damage and death, ship flight and docking,
  plus a crash guard around some engine functions. Another plugin that patches
  the same function in an incompatible way (an overwrite instead of a chained
  detour) can break one of them. Unknown which existing plugins do.
- PULSAR does not hook rendering: its in-game overlay code is not installed, so
  overlays (Steam, ReShade, performance overlays) don't share a hook with it.
- PULSAR installs no crash handler of its own (no unhandled-exception filter), so it
  doesn't fight a crash logger for that.

### Starfield Community Patch

**Unknown: not tested.** What is known: the Community Patch is a plugin that fixes
records, and PULSAR's plugin overrides no records, so the two don't conflict as
plugins. Whether co-op sync behaves with the patched quests is unknown. If both
players use it, both need the same version.

### DLC (Shattered Space and the Creations)

**Unknown: not tested.** The test copies have the base game only. Derived from the
form ID rule above: if the host's save uses DLC content, the joiner needs the same
DLC.

## Known incompatibilities

None found yet, because nothing but PULSAR has been tested. If you find one, report it
with both players' `Plugins.txt` and `Data\SFSE\Plugins` listing and both
`PULSAR.log` files.
