# Procedural Ecosystem Loot Ecology

This project already generates a **deterministic ecosystem field** per floor (biomes + ecotones).  
Round 210 extends that idea into **loot**, so biome regions don't just *look* different — they also feel different in the small decisions that matter moment-to-moment.

## What was added

### 1) Ecosystem-biased weapon brands (ItemEgo)

When a melee weapon rolls an **ego/brand** (Flaming/Venom/Webbing/Corrosive/Dazing/Vampiric), the weight table is now nudged by the **local ecosystem** at the spawn position.

This is a *nudge* system, not a hard rule:

- Room type and substrate still matter (e.g., Armories still skew “martial”, Moss/Dirt still skew Venom/Webbing).
- Ecosystems add a small final bias so the same room can feel different depending on where it sits in the biome map.

Implemented via pure helpers in `src/ecosystem_loot.hpp`:
- `ecoWeaponEgoWeightDelta(EcosystemKind, ItemEgo)` – signed weight delta
- `ecoWeaponEgoChanceMul(EcosystemKind)` – a mild multiplier on the *chance* of any ego appearing

### 2) Ecosystem loot caches near biome seeds

Biome seeds are already used to place:
- essence shards (crafting byproducts)
- stationary harvest nodes (Spore Pod / Crystal Node / etc.)

This round adds **small “loot caches”** near some seeds:
- 2 themed items (plus an occasional 3rd)
- placed on clean floor tiles inside the correct ecosystem region
- avoids shops and stair landing zones

Examples:
- **Fungal Bloom:** antidotes / regeneration + venom-flavored utility  
- **Crystal Garden:** rune tablets / wands + identification / vision  
- **Rust Veins:** lockpicks / keys + knock / corrosion-flavored utility  
- **Flooded Grotto:** healing + mapping / vision

**Important:** cache placement uses a derived RNG stream so it **does not perturb** the main loot RNG.  
This keeps the rest of the floor’s item rolls stable even if cache tuning changes later.

## Why this is useful

- **More readable world-building:** the same “loot type” now has local flavor.
- **Better emergent decisions:** players can lure fights (and scavenge) in regions that fit their plan.
- **Safer tuning:** because it’s bias-based, you can tweak deltas without rewriting item tables.

## Tuning knobs

All tuning for brands lives in `src/ecosystem_loot.hpp`:
- keep deltas small (roughly within ±25) so tables remain recognizable
- keep chance multipliers close to 1.0 (0.85–1.15 range)

Cache content is defined in `Game::spawnItems()` and can be adjusted per-ecosystem.

## Tests

`tests/test_main.cpp` includes a guardrail test:
- `ecosystem_weapon_ego_loot_bias` verifies the direction/sign of key weight deltas and that the chance multipliers remain mild.

