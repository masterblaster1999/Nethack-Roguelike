# Procedural Ecosystem Hazards & Ambience

This project now has a deterministic **ecosystem / biome seed** field (see `Dungeon::ensureMaterials()`), and this document describes two systems layered on top:

1. **Ecosystem trap clusters** (gameplay)
2. **Ecosystem ambient particles** (visual-only)

Both are designed to be **deterministic** per floor and to avoid “butterfly effects” where adding flavor changes unrelated generation outcomes.

---

## 1) Ecosystem trap clusters (gameplay)

Traps already spawn via a small budgeted system (`Game::spawnTraps()`): cache guards, corridor gauntlets, traffic traps, then baseline scatter.

The ecosystem trap cluster layer adds **small regional patches** of traps that match the local ecosystem. The intent is not to increase global difficulty, but to make biome regions *feel* distinct:

- **FUNGAL BLOOM**: webs + confusion gas (and deeper: poison gas)
- **CRYSTAL GARDEN**: alarm wards + occasional teleport runes
- **BONE FIELD**: spike fields + rare trapdoors (deeper)
- **RUST VEINS**: spikes + corrosive gas (deeper)
- **ASHEN RIDGE**: lethe mist + confusion; rare boulder hazards (deeper)
- **FLOODED GROTTO**: misty alarms + webs / occasional teleport (deeper)

### Determinism & isolation

To prevent ecosystem flavor from perturbing other trap setpieces, these clusters use an **isolated RNG stream** derived from the floor’s level seed.

### Budget behavior

Cluster traps **count toward the existing trap budget** (`targetCount`). That means:

- If clusters place a few traps early, baseline scatter places fewer.
- Net density stays roughly stable.

---

## 2) Ecosystem ambient particles (visual-only)

The renderer already has “ambient emitters” (fountains, altars, biolum) implemented using a deterministic **phase-crossing** test, which keeps emission stable across frame rates.

Ecosystem tiles now also emit subtle per-biome particles:

- Fungal Bloom: greenish spore smoke
- Crystal Garden: cold motes / twinkles
- Bone/Rust: faint dust
- Ashen Ridge: heavier ash + rare ember flickers
- Flooded Grotto: tiny droplets / spray

### Performance + stability

- Only **visible** tiles are considered.
- Only **1 in 4** eligible ecosystem tiles can emit (stable per tile), keeping counts controlled even in large single-biome rooms.
- Particles are visual-only and do not affect gameplay.
