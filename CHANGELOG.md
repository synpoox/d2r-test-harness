# Changelog

## Unreleased

- The virtual pad reaches plugins in any folder under the game folder, mod-scoped plugins included.
- The pad moves focus in controller menus after a click: a pad change later than the last cursor input hands the cursor back to the real one.
- Virtual XInput pad and held keys, driven by `tools/pad.py`; other plugins loaded before the harness read the held keys too.
- First version: the harness protocol's cursor, modifiers, focus and console, with the game's sound muted.
