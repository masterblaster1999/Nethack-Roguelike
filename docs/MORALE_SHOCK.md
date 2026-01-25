# Morale Shock & Intimidation

The game already has a **Fear** status effect that causes monsters to **flee from the player**.

Round 147 adds a new emergent way for fear to occur: **witnessed kills can break enemy morale**.

## Trigger conditions

A morale shock check occurs when:

- A **hostile** creature dies (not-friendly and not the Player).
- The killer is the **player** or a **friendly companion**.

Only **hostiles** are eligible to panic. (In this codebase, Fear is implemented as “fear of the player”, so we keep the mechanic tightly scoped to avoid strange cross-faction behavior.)

## Who can panic

A hostile witness must:

- Be **alive**.
- Be within a small **radius** of the kill tile (Chebyshev distance).
- Have clear **line of sight** to the kill tile.

## The morale save

Each witness makes a deterministic “morale save”:

- Roll a deterministic **d20** (no RNG consumption).
- Add a **bonus**.
- If the result is **below the panic DC**, the witness panics and receives **Fear** for several turns.

Fear duration scales with how badly the witness failed the save.

### Panic DC

The panic DC is driven by how terrifying the fallen enemy was:

- Scales with the victim’s **procedural rank tier** (Normal → Elite → Champion → Mythic).
- Increases if the victim had the **Commander** affix.
- Increases for **Minotaurs** (boss-like presence).
- Slightly increases for large **overkill** (dramatic kills).
- Decreases for “assassination-style” melee kills (you were **sneaking** or **invisible** before striking), representing a quieter kill that is less likely to cause a wide panic cascade.

### Witness bonus

The witness bonus represents toughness and discipline:

- Scales with the witness’s own **procedural rank tier**.
- Gains from nearby **Commander aura**.
- Guards and Shopkeepers get an extra bonus.
- Mindless undead (Zombies, Ghosts, Skeleton Archers) get a large bonus (mostly immune to fear-of-player effects).
- Badly wounded witnesses take a small penalty.

## Player feedback

- Panic messages only appear when at least one panicking enemy is **visible** to the player (no telepathic off-screen tells).
- A small particle ping appears at the kill tile if the death location is visible.

## Design goals

- Make combat more dynamic: big kills can create openings by scattering weaker foes.
- Reward stealth: assassinations are less likely to trigger broad panic cascades.
- Keep replays stable: morale saves are deterministic and do not consume the main RNG stream.
