# d2r-test-harness

A test-only D2RLoader plugin. It is the plugin side of d2rAI-toolkit's harness protocol
(`docs/testing.md` there), so the toolkit's `game_ctl.py` and `game_checks.py` can click, hold modifiers and send console
lines in a game slot without the real mouse or keyboard.

**Never ship this to players.** Install it only into a game slot. It needs no changes to the plugins under test.

## What it does

It stays idle unless `d2r-testharness.txt` sits next to the DLL (`game_ctl.py harness on` creates it). When on, it:

- answers the game's cursor, key and focus queries by swapping USER32 entries in the import tables of the game image
  and `D2RCore.dll`, so it uses no game addresses and needs no update for a new build;
- runs console lines on the UI thread and appends each line and `handled` or `refused` to
  `d2r-testharness-console.txt`. A command's own output goes to that plugin's log, not to this file;
- mutes the game's own audio sessions, so test runs are silent. Other programs are not affected.

Console command `testharness` prints the state.

## Build

```powershell
git clone --recurse-submodules <this repository>
powershell -ExecutionPolicy Bypass -File build.ps1
```

The output is `build\Release\test-harness.dll`. Copy it to `<slot>\d2rloader\plugins\`.

## Slot config

The slot's `modkit-testing.toml` keeps the protocol's default numbers and sets these names:

```toml
[mod]
plugins_subdir = "../../d2rloader/plugins"
plugin_dlls = ["test-harness.dll"]

[harness]
shm_name = "Local\\d2r-testharness-block-{pid}"
word_file = "d2r-testharness.txt"
console_file = "d2r-testharness-console.txt"
```

A slot needs a mod for its own save folder: without one the game writes to the player's real saves. An empty mod is
enough: `mods\<Name>\<Name>.mpq\modinfo.json` with `name` and `savepath`, and `default_mod = "<Name>"` in the slot's
`d2rloader.toml`.

A fresh slot opens on the calibration screens. Add `"First Time": 1`, `"Tutorial": 1` and `"Master Volume": 0` to
`Settings.json` in the slot's save folder once.
