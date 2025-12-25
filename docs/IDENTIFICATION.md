# Item Identification

This build adds a lightweight NetHack-style identification system for **potions** and **scrolls**.

## How it works

- At the start of every run, each potion/scroll type is assigned a randomized **appearance**:
  - Potions show as things like `RUBY POTION`, `SMOKE POTION`, etc.
  - Scrolls show as `SCROLL 'ZELGO'`, `SCROLL 'KLAATU'`, etc.
- When you **use** a potion/scroll for the first time, you learn its real name for the rest of the run.
- Reading a **Scroll of Identify** reveals the real name of **one random unidentified** potion/scroll in your inventory.

## Disabling it

If you prefer the older behavior (all items always show true names), set:

```ini
identify_items = false
```

in `procrogue_settings.ini`.