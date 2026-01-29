# Round 208 Notes

## Procedural Ecosystem Resource Nodes

This round introduces **procedural ecosystem “resource nodes”**: stationary props you can harvest for Essence Shards and small bonuses, at the cost of biome-themed backlash hazards.

### What you’ll see
- **Spore Pods** in fungal regions (confusion gas risk)
- **Crystal Nodes** in crystal regions (loud, grants short shield)
- **Bone Piles** in bone regions (mild amnesia shock)
- **Rust Vents** in rust regions (corrosive gas + corrosion)
- **Ash Vents** in ash regions (fire field + burn)
- **Grotto Springs** in flooded regions (heals/cleanses + clears nearby gas)

### How it plays
- Walk onto the node and press **CONFIRM**.
- Nodes have **1–3 taps** (stored in `Item::charges`).
- Each harvest yields at least one Essence Shard plus a rare small bonus item.

### Implementation highlights
- New `ItemKind` values are **append-only** and therefore save-safe.
- Nodes are flagged `ITEM_FLAG_STATIONARY` and excluded from pickup/autopickup.
- Spawn placement uses a **derived RNG** so core loot generation remains stable.
