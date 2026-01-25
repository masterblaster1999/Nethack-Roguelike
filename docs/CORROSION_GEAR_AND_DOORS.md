# Corrosion: Gear Pitting and Door Erosion

This project already models **Corrosive Gas** as a persistent hazard cloud (plus a timed **Corrosion** status effect on entities). This patch extends the system so corrosive environments feel like a *long-term dungeon force* rather than only a short-term HP tax.

## Gameplay changes

### 1) Gear pitting during Corrosion ticks

When an entity has **Corrosion**, it still takes the usual stinging damage tick every other turn. On the same damage tick, corrosive exposure can also *pit exposed equipment*:

- The **player** can lose **1 enchantment** on an equipped piece of gear (**armor**, **melee weapon**, or **ranged weapon**).
- **Monsters** can also have their gear pitted (this mostly matters for what they drop on death).
- **Shielding** prevents this secondary gear pitting.
- **Blessed** gear resists slightly; **cursed** gear is more vulnerable.
- **Artifacts are immune**.
- Enchantment is clamped to a minimum of **-3**.

This makes Corrosion more “NetHack-like” in consequences, while staying deliberately mild and readable.

### 2) Doors can erode in strong Corrosive Gas

Closed/locked doors that are exposed to **strong Corrosive Gas** (on either side of the door) can degrade over time:

- `DoorLocked` can become `DoorClosed` (the lock fails).
- `DoorClosed` can become `DoorOpen` (the door “sizzles open”).

Laboratory door sealing types affect the thresholds:

- **Airlocks** are sturdier (higher thresholds, lower max chance).
- **Vented** lab doors are a bit weaker.

When a door becomes open this way, it triggers the same gas/pressure puff logic as a normal door opening, so a sealed room can suddenly vent fumes into a corridor.

### 3) Ground gear can slowly pit

Weapons/armor left lying in **very strong** Corrosive Gas can (rarely) lose 1 enchantment over time. This is intentionally rare and only checks every other turn, but it discourages leaving loot piles in acid.

## Balance notes

- Door erosion is tuned to require **high gas intensity** and has a **low per-turn chance**, so it won’t routinely delete doors.
- Gear pitting is tied to the **corrosion damage tick** and can be avoided with **Shielding** or by leaving corrosive areas quickly.
- Visibility only affects messaging; the underlying RNG behavior remains deterministic for save/load.

## Implementation notes

- Player + monster gear pitting is implemented in `Game::applyEndOfTurnEffects()` alongside the existing corrosion damage tick.
- Door + ground-item erosion is applied after gas diffusion and vented-door seepage, so it reacts to the hazard map that will exist next turn.

**Primary implementation file:** `src/game_spawn.cpp`
