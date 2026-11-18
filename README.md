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
- feeds a [virtual pad and held keys](#virtual-pad-and-held-keys) from a state file;
- mutes the game's own audio sessions, so test runs are silent. Other programs are not affected.

Console command `testharness` prints the state.

## Virtual pad and held keys

A scripted controller and held keys, with no driver (no ViGEmBus) and no admin. D2R 93847 polls `XInputGetState` about
100 times a second (it loads `XINPUT9_1_0.dll` at start, `XInput1_4.dll` a few seconds later), so the plugin swaps that
export, `XInputGetStateEx` (ordinal 100), `XInputGetCapabilities`, `XInputSetState` and `XInputGetKeystroke` in the
same import-table scan it uses for the cursor. Exports are found by name in every loaded XInput DLL, never by address.
Held keys answer `GetAsyncKeyState` and `GetKeyState`, which is how core-ui reads Ctrl.

The pad needs `d2r-testharness.txt` (game_ctl writes it at every start); the plugin then polls the pad file every 20 ms.
The pad is connected exactly while `d2r-testharness-pad.txt` exists next to the DLL, and the plugin deletes a leftover
pad file when it loads, so a new run never starts with a stale pad or held keys. With no pad file the real XInput answers.

```powershell
$env:MODKIT_TESTING_CONFIG = "C:\path\to\modkit-testing.toml"      # --slot N reads game.slots_dir from it
py -3 tools/pad.py --slot 3 reset                              # connect an idle pad (the game switches to controller UI)
py -3 tools/pad.py --slot 3 press A [200ms]                    # tap; also UP DOWN LEFT RIGHT START BACK LS RS LB RB X Y B GUIDE LT RT
py -3 tools/pad.py --slot 3 hold LB / release LB|all           # keep a button down
py -3 tools/pad.py --slot 3 stick L 0.0 1.0 500ms              # left stick x y in -1..1 (y up), for 500 ms, then centred; omit the time to hold
py -3 tools/pad.py --slot 3 key hold ctrl / key release ctrl|all   # held keys: ctrl shift alt space tab esc, letters, f1..f24, vk<N>
py -3 tools/pad.py --slot 3 status | off                       # `off` removes the file: the pad is gone, the real XInput answers again
```

`--plugins DIR` replaces `--slot`. Console command `testharness` prints the pad state and how often the game called each
XInput function; the plugin log lists input-related modules as they load. Tests: `py -3 -m unittest tests.test_pad`.

Limits: a mouse move or click makes the game fall back to the mouse UI (send pad input after it, and `release` the
mouse); held keys answer only the key-state queries, not `WM_KEYDOWN`, so they suit modifiers; rumble is dropped; only
pad 0 exists; `GetAsyncKeyState`'s low bit (pressed since the last call) is never set, and a press shorter than ~20 ms
can be missed. The caller must hold the slot lock, as for every other harness command (`pad.py` does not check it).

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
