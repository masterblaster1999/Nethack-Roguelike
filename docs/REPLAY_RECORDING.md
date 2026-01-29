# Replay Recording

ProcRogue can record your run inputs to a **replay file** (`.prr`) and play them back later.
This is useful for:

- Sharing interesting runs
- Reproducing bugs
- Debugging determinism (with per-turn state hashes)

## Record from the command line

Start a recording immediately on launch:

- `--record`  
  Writes `procrogue_replay_<timestamp>.prr` into your data directory.

- `--record my_run.prr`  
  Writes to a specific path (relative paths are resolved inside the data directory).

Optional determinism helpers:

- `--record-hashes` / `--no-record-hashes`
- `--record-hash-interval <n>` (default: `1`)

## Record in-game

You can start/stop recording at any time using the extended command prompt:

- `record [path]` (alias: `rec`)  
  Starts recording a replay to an optional filename.

- `stoprecord` (alias: `stoprec`)  
  Stops recording and closes the replay file.

While a replay is recording, the HUD shows a small `REC` tag.

## Play back a replay

Launch with:

- `--replay <path>`

If the replay contains state-hash checkpoints, playback can verify determinism automatically
(the default). If a mismatch is detected, playback stops and prints a divergence message.
## Interactive replay playback controls

When you launch the game with `--replay path/to/file.nhr`, normal input is driven by the recorded replay events. To prevent accidental desync, **most gameplay inputs are ignored while playback is active**, but you can still control the playback itself.

Default keybinds (rebindable):

- **Pause / resume:** Space (`replay_pause`)
- **Step to next recorded event:** `.` / Right Arrow (`replay_step`)
- **Faster:** `+` / `=` (`replay_speed_up`)
- **Slower:** `-` (`replay_speed_down`)
- **Stop playback (unlock input):** Esc (`cancel` while replay is active)

Controller shortcuts (during replay playback):

- **Start:** pause / resume
- **A:** step
- **LB / RB:** slower / faster
- **B:** stop playback (unlock input)

While playback is active, the HUD shows a `REPLAY` tag with the current speed and a timeline (and `PAUSED` when paused).

Notes:

- Playback speed scales simulation time too, so **auto-travel and other time-based systems keep up**.
- Stopping playback unlocks input immediately (you can continue playing from the current state). If hash verification was enabled, it is disarmed when playback is stopped so you can safely take over.
