# Electric Conduction (Spark Arcs)

This project already has **wind-drifted physical projectiles**, deterministic **terrain materials** (STONE/BRICK/METAL/CRYSTAL/etc.), and a wide variety of ranged/spell effects.

This document describes the **conductive spark** mechanic added in Round 145: when a **Spark** projectile impacts something, it can occasionally **arc** (chain lightning) to nearby targets, with the arc behavior biased by the dungeon’s substrate materials.

## What counts as "conductive"?

A tile is treated as **conductive** if any of the following are true:

- Its **terrain material** is **METAL** or **CRYSTAL** (from the existing deterministic material field).
- Its **tile type** is **CHASM**.

> Note: in this roguelike’s ruleset, chasms also represent *surface water basins* in the camp hub and other "water-like" voids, so they serve as a strong conductor for electricity.

We also compute a **conduction tier** at the point of impact:

- **Tier 2**: the impact tile itself is conductive.
- **Tier 1**: the impact tile is not conductive, but a **4-neighbor** adjacent tile is.
- **Tier 0**: no conductive adjacency.

## When do arcs happen?

When a Spark projectile hits an entity or a wall/door corner, the game rolls an **arc chance**:

- Wands (the `wandPowered` damage profile) are significantly more likely to arc.
- Higher conduction tiers increase arc chance.
- A **critical hit** increases arc chance (so sparks can *rarely* arc even on non-conductive stone).

Arcs are capped to a modest maximum probability and are intended to be **spicy but not reliable**.

## How are arc targets selected?

If an arc triggers, we perform up to a small number of "jumps" (more on strong conductors + wand sparks).

For each jump:

- Eligible targets are chosen by side:
  - **Player-side** sparks will only arc to **hostiles** (avoid zapping allies/pets).
  - **Monster-side** sparks will only arc to the **player side** (player + friendly companions).
- The candidate must be within a limited radius.
- The candidate must have a **clear line** (walls/closed doors block; arcs cannot "skip" over a nearer body).
- The best candidate is chosen via a **deterministic score**:
  - More conductive tiles along the arc line increases score.
  - Shorter distance increases score.
  - Standing on a conductive tile is a small bonus.

This keeps the result coherent and reproducible for a given run, without introducing a bunch of extra randomness.

## Damage + status

Each jump deals **reduced Spark damage** (falloff per hop), and the victim has a small chance to be briefly **stunned** using the existing `confusionTurns` effect.

## FX

Each arc jump spawns a short **Spark projectile FX** along the chosen arc line, so the chain is visually readable.
