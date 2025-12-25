# Changelog

## 0.3.0

### New gameplay
- **NetHack-style item identification**: potions + scrolls start unidentified each run, with randomized appearances.
  - Using an item identifies that item type for the rest of the run.
  - New item: **Scroll of Identify** (reveals one random unknown potion/scroll in your inventory).
- **New monster pressure**:
  - **Spiders** can **web** the player (prevents movement for a few turns).
  - **Wizards** can occasionally **blink** (teleport) to reposition.

### UI / QoL
- Inventory, look mode, pickup/drop messages now respect identification (unknown items show their appearance label).
- HUD now shows compact **status effect tags** (POISON / WEB / REGEN / SHIELD / HASTE / VISION).

### Settings
- New key: `identify_items = true/false` (see `docs/SETTINGS.md`).

### Save compatibility
- Save version bumped to **v6**.
  - v5 and older saves still load (items will appear fully known, matching older behavior).
