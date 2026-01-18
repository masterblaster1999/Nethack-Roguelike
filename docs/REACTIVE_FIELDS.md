# Reactive Fields (Fire / Gas Chemistry)

ProcRogue has a few *persistent* environmental field hazards that live on the dungeon grid:

- **Confusion Gas** (lingering cloud; causes confusion)
- **Poison Gas** (lingering cloud; causes poison)
- **Fire** (lingering flames; causes burning)

As of the latest patch, these fields can now *interact* in small but tactically meaningful ways.

## Fire burns away gas

If a tile contains **Fire** and **Gas** at the same time:

- **Fire will gradually consume the gas** on that tile.
- Confusion gas is only “cleaned up” (reduced) — it is not explosive.

This gives fire effects (fireballs, lingering flames, etc.) a new utility: **clearing dangerous gas corridors**.

## Poison gas can ignite

Dense **Poison Gas** can occasionally ignite when it overlaps **strong Fire**.

When this happens:

- The poison cloud is **consumed** in the blast area.
- A brief **flash-fire** explosion deals a small amount of damage and **ignites** entities caught inside.
- The blast leaves **lingering flames** for a short time.
- It creates a loud **noise** (can attract monsters).

You’ll also see an **IGNITION RISK** hint in LOOK mode when a visible tile contains both **POISON GAS** and **FIRE**.

## Tactical implications

- Ignitions are intentionally **rare** and capped to avoid chain reactions.
- Fire can be used to **deny** poison gas traps by clearing clouds.
- Be careful with fire spells near dense poison gas — an ignition can catch *you* too.

