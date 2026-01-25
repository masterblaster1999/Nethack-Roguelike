# Procedural Weapon Ego Ecology

This project has a lightweight “ego” system for melee weapons: rare branded weapons that provide small, flavorful combat procs.

In Round 155, the ego system was expanded in two major ways:

1. **New egos** were added (append-only, save compatible).
2. Ego generation now has a **procedural ecology**: brands are biased by a per-floor theme, room type, and substrate material. This makes loot feel like it “belongs” to the places you find it, while staying unpredictable.

---

## Ego brands

Egos are currently limited to the core melee weapons:

- **DAGGER**
- **SWORD**
- **AXE**

### Existing

- **FLAMING**: chance to apply **burn** on hit.
- **VENOM**: chance to apply **poison** on hit.
- **VAMPIRIC**: heals the attacker for a small amount based on damage dealt.

### New (Round 155)

- **WEBBING**: chance to apply **web** on hit (prevents movement unless the victim can phase).
- **CORROSIVE**: chance to apply **corrosion** on hit (temporary defensive penalty + corrosion system hooks).
- **DAZING**: chance to apply **confusion** on hit (short “daze” effect).

All new egos are appended to `ItemEgo` to preserve save compatibility.

---

## Procedural ego ecology

When an ego roll succeeds, the ego isn’t chosen from a flat distribution. Instead, the game builds a small weighted table and then applies *deterministic biases*:

### 1) Per-floor dominant ego

Each floor chooses a **dominant ego** as a deterministic function of:

- `runSeed`
- `depth`

This bias does **not** consume RNG, so it stays stable even if generation order changes.

The dominant ego doesn’t guarantee anything; it simply increases the weight of one brand so that (occasionally) a floor will “lean” into a theme.

### 2) Room type bias

Room theming nudges brands toward setpieces that make intuitive sense:

- **Laboratory** → CORROSIVE / DAZING
- **Library / Shrine** → DAZING
- **Lair** → WEBBING / VENOM
- **Armory** → FLAMING
- **Vault / Treasure / Secret** → VAMPIRIC (plus a small FLAMING nudge)

### 3) Terrain material bias

Substrate materials (STONE / MOSS / METAL / etc.) apply small nudges:

- **Obsidian/Basalt** → FLAMING
- **Moss/Dirt** → VENOM / WEBBING
- **Bone** → VAMPIRIC / VENOM
- **Metal** → CORROSIVE
- **Crystal / Marble** → DAZING

These are deliberately small: materials are primarily visual + acoustics/scent, but this adds subtle gameplay flavor.

### 4) Shop and monster bias

- Shops tilt slightly toward **premium mid/rare** brands.
- Monster gear reduces the heaviest “hard-disable” egos (WEBBING/DAZING) to avoid frustration.

---

## Key implementation files

- `src/items.hpp`
  - `ItemEgo` append-only expansion
  - `egoPrefix()` for display names
  - `egoValueMultiplierPct()` for shop valuation

- `src/game_spawn.cpp`
  - `rollWeaponEgo(...)` now includes run seed + room + substrate ecology

- `src/combat.cpp`
  - Webbing / Corrosive / Dazing on-hit procs

---

## Tuning notes

The intent is for egos to remain **rare spice**, not a mandatory power band:

- Ego chance is still capped.
- Ego effects are short and proc-based.
- Artifact powers remain the “big ticket” deterministic gear effects.
