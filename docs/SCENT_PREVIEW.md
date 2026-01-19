# Scent Trail Preview (LOOK Lens)

The game maintains a **scent field** on the map: each walkable tile stores a value (0–255) representing how recently/strongly the player has left a trail there. This is used by **smell-capable** monsters to track you around corners.

The **Scent Trail Preview** is a UI-only LOOK lens that lets you visualize this field as a heatmap and a small flow/vector overlay.

## How to use

- Enter LOOK mode (`look` action; default `l`).
- Toggle the lens:
  - **Default keybind:** `ctrl+s` (`bind_scent_preview`)
- While the lens is open:
  - Press **`[` / `]`** to adjust the **cutoff** (minimum scent intensity to render).

This lens never consumes a turn.

## What you see

- **Heatmap (sepia tint):**
  - Brighter/more opaque tiles indicate **stronger/fresher** scent.
  - Only **explored** tiles are drawn.

- **Flow arrows (vector field):**
  - Each arrow points toward a neighboring tile with **stronger** scent.
  - This approximates the direction a smell-tracking monster would tend to move when following your trail.

- **Player tile outline:**
  - Your current tile is outlined so you can quickly locate the freshest deposit.

## Why it’s useful

- **Plan sneaking routes:** Sneak mode reduces how much fresh scent you deposit, and the preview makes the difference visible.
- **Use doors/corridors to break trails:** Since the scent field only spreads through walkable tiles, closing doors and changing routes can reshape the gradient.
- **Understand tracking behavior:** The arrows help explain why a monster chooses a particular corner or hallway when it has lost line-of-sight.

## Notes

- The cutoff defaults to a value that roughly matches the AI’s “trackable” threshold, so the lens emphasizes scent that matters for pursuit.
- The lens is intentionally conservative: it never reveals unexplored tiles.
