# Procedural Rune Wards

This project already has **warding words**: special floor engravings that can make some monsters hesitate while you stand on them.

Round 212 adds **Rune Wards**: *Ward-form* procedural rune spells (typically from **Rune Tablets**) now **etch an elemental ward inscription underfoot** when cast. These rune wards plug directly into the existing ward system (AI hesitation, durability wear, LOOK readout, etc).

## What counts as a Rune Ward

Rune wards are recognized by the engraving text prefix:

- `RUNE FIRE`
- `RUNE:FIRE`
- `RUNE OF FIRE`

…and the parser intentionally allows decorations after the element token, so proc spells can embed a sigil:

- `RUNE FIRE: KAR-THO-RAI`

Only the **prefix** is parsed; anything after the element token is treated as flavor text.

## Casting behavior

When you cast a **proc spell** whose `form == WARD`:

- You still receive the existing self-buff (haste / levitation / invis / detect / regen / shield depending on element).
- The game attempts to **inscribe a rune ward engraving** on the tile you're standing on:
  - It will **not overwrite** a non-ward, non-graffiti engraving (player notes, sigils, etc).
  - It can refresh/replace an existing **ward** or **graffiti** on that tile.
  - Durability ("uses") is derived from tier + focus + mods (Focused/Lingering increase; Volatile slightly reduces).

The resulting engraving is a standard ward (`isWard=true`) and is therefore shown in LOOK mode and consumed/worn by monster interactions exactly like other wards.

## Elemental affinities

Rune wards do **not** repel every monster equally; each element targets a different set:

- **RUNE RADIANCE**: undead + wizards
- **RUNE SHADOW**: most living threats (but *not* undead); shopkeepers/guards/minotaurs ignore it
- **RUNE ARCANE**: wizards, mimics, leprechauns, nymphs
- **RUNE SHOCK**: goblins, orcs, kobold slingers
- **RUNE FIRE**: slimes, spiders
- **RUNE FROST**: bats, wolves, snakes
- **RUNE STONE**: ogres, trolls, minotaurs
- **RUNE WIND**: bats, ghosts
- **RUNE VENOM**: wolves, snakes
- **RUNE BLOOD**: wolves, snakes, spiders

These affinities are intentionally “gamey” rather than simulation-perfect: the goal is to make ward spells feel distinct and to give Rune Tablets a new procedural identity beyond raw damage/buffs.

## Player engraving

Because rune wards are parsed from text, players can engrave them manually too:

- Engrave `RUNE SHADOW` to make a broad “fear ward” (weaker per creature, but affects many).
- Engrave `RUNE RADIANCE` as an anti-undead circle.
- Add flavor text and it still works: `RUNE FIRE: KAR-THO-RAI`.

(Engraving durability still depends on the player's current engraving tool/weapon, as before.)

## Procedural placement: Ancient Rune Wards

Round 213 extends Rune Wards into **procedural level flavor + tactics**:

- On dungeon floors (non-camp) at **depth 3+**, the generator may carve a *tiny* number of **Ancient Rune Wards**.
- These are placed at **high-intensity leyline nodes** (local maxima in the leyline field), biased toward junctions/open areas.
- Placement avoids obvious "free win" spots:
  - kept away from stairs landings,
  - not inside Shops / Shrines / Vaults / Secret rooms,
  - never overwrites existing graffiti/sigil engravings.
- Ancient wards have **finite durability** (strength) like normal wards, so they are *rally points*, not permanent safe zones.

The text format is still the same ward parser understands, e.g.:

- `RUNE ARCANE: THAL-UR-NOK`

The trailing sigil is generated for flavor; only the prefix (`RUNE ARCANE`) is parsed.

## Leyline rune caches

Sometimes, an Ancient Rune Ward will have a small cache nearby:

- A **Rune Tablet** is spawned within a couple tiles of the ward.
- The tablet's proc spell is **element-matched** to the ward (bounded search over proc IDs), so the cache feels like it belongs to the node.

These caches are intentionally sparse and capped so they add exploration "breadcrumbs" without flooding the early game with tablets.

