"""Virtual game pad and held keys for the test harness plugin (test-only).

    py -3 tools/pad.py --slot 3 press A            # tap a button (100 ms)
    py -3 tools/pad.py --slot 3 hold LB            # keep it down until `release`
    py -3 tools/pad.py --slot 3 stick L 0.0 1.0 500ms   # push the left stick up for 500 ms, then centre it
    py -3 tools/pad.py --slot 3 key hold ctrl      # held Ctrl (GetAsyncKeyState / GetKeyState answer "down")
    py -3 tools/pad.py --slot 3 key release all
    py -3 tools/pad.py --slot 3 status | reset | off

The plugin reads d2r-testharness-pad.txt next to test-harness.dll. The file's existence is what makes the pad
"connected"; `off` removes it. Stick Y is up-positive, like XInput. See the README.
"""
from __future__ import annotations

import argparse
import os
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

PAD_FILE = "d2r-testharness-pad.txt"

BUTTONS = {  # XINPUT_GAMEPAD_* masks
    "UP": 0x0001, "DOWN": 0x0002, "LEFT": 0x0004, "RIGHT": 0x0008,
    "START": 0x0010, "BACK": 0x0020, "LS": 0x0040, "RS": 0x0080,
    "LB": 0x0100, "RB": 0x0200, "GUIDE": 0x0400, "A": 0x1000, "B": 0x2000, "X": 0x4000, "Y": 0x8000,
}
TRIGGERS = ("LT", "RT")
KEYS = {  # Windows virtual-key codes
    "ctrl": 0x11, "lctrl": 0xA2, "rctrl": 0xA3, "shift": 0x10, "lshift": 0xA0, "rshift": 0xA1,
    "alt": 0x12, "lalt": 0xA4, "ralt": 0xA5, "space": 0x20, "tab": 0x09, "esc": 0x1B,
}


class PadError(ValueError):
    pass


@dataclass
class State:
    buttons: int = 0
    lt: int = 0
    rt: int = 0
    lx: int = 0
    ly: int = 0
    rx: int = 0
    ry: int = 0
    keys: set[int] = field(default_factory=set)

    def line(self) -> str:
        keys = ",".join(str(k) for k in sorted(self.keys))
        return (f"buttons=0x{self.buttons:04X} lt={self.lt} rt={self.rt} lx={self.lx} ly={self.ly} "
                f"rx={self.rx} ry={self.ry} keys={keys}\n")

    @classmethod
    def parse(cls, text: str) -> "State":
        f = dict(re.findall(r"(\w+)=(\S*)", text))
        keys = {int(k) for k in f.get("keys", "").split(",") if k}
        return cls(int(f.get("buttons", "0"), 0), int(f.get("lt", 0)), int(f.get("rt", 0)), int(f.get("lx", 0)),
                   int(f.get("ly", 0)), int(f.get("rx", 0)), int(f.get("ry", 0)), keys)


def parse_duration(text: str) -> float:
    """'500ms', '2s' or a bare number of seconds -> seconds."""
    m = re.fullmatch(r"(\d+(?:\.\d+)?)(ms|s)?", text.strip().lower())
    if not m:
        raise PadError(f"bad duration {text!r} (use 500ms or 2s)")
    return float(m.group(1)) / (1000 if m.group(2) == "ms" else 1)


def axis(value: str) -> int:
    v = float(value)
    if not -1.0 <= v <= 1.0:
        raise PadError(f"stick value {value} is outside -1..1")
    return round(v * 32767)


def button_name(name: str) -> str:
    n = name.upper()
    if n not in BUTTONS and n not in TRIGGERS:
        raise PadError(f"unknown button {name!r} (one of {', '.join(list(BUTTONS) + list(TRIGGERS))})")
    return n


def key_code(name: str) -> int:
    n = name.lower()
    if n in KEYS:
        return KEYS[n]
    if len(n) == 1 and n.isalnum():
        return ord(n.upper())
    if re.fullmatch(r"f(\d{1,2})", n) and 1 <= int(n[1:]) <= 24:
        return 0x6F + int(n[1:])
    if re.fullmatch(r"vk(\d{1,3})", n):
        return int(n[2:])
    raise PadError(f"unknown key {name!r}")


def set_button(s: State, name: str, down: bool) -> None:
    n = button_name(name)
    if n == "LT":
        s.lt = 255 if down else 0
    elif n == "RT":
        s.rt = 255 if down else 0
    elif down:
        s.buttons |= BUTTONS[n]
    else:
        s.buttons &= ~BUTTONS[n]


def plan(argv: list[str], state: State) -> list[tuple[State, float]]:
    """A command as steps: (state to write, seconds to keep it). Pure, so it is unit tested."""
    if not argv:
        raise PadError("no command")
    cmd, args = argv[0].lower(), argv[1:]
    s = State(**{**state.__dict__, "keys": set(state.keys)})
    if cmd == "press":
        if not 1 <= len(args) <= 2:
            raise PadError("press <button> [duration]")
        down = State(**{**s.__dict__, "keys": set(s.keys)})
        set_button(down, args[0], True)
        return [(down, parse_duration(args[1]) if len(args) == 2 else 0.1), (s, 0.0)]
    if cmd == "hold":
        if len(args) != 1:
            raise PadError("hold <button>")
        set_button(s, args[0], True)
        return [(s, 0.0)]
    if cmd == "release":
        if len(args) != 1:
            raise PadError("release <button|all>")
        if args[0].lower() == "all":
            s.buttons = s.lt = s.rt = 0
            s.lx = s.ly = s.rx = s.ry = 0
        else:
            set_button(s, args[0], False)
        return [(s, 0.0)]
    if cmd == "stick":
        if len(args) not in (3, 4) or args[0].upper() not in ("L", "R"):
            raise PadError("stick L|R <x> <y> [duration]")
        x, y = axis(args[1]), axis(args[2])
        moved = State(**{**s.__dict__, "keys": set(s.keys)})
        if args[0].upper() == "L":
            moved.lx, moved.ly = x, y
        else:
            moved.rx, moved.ry = x, y
        if len(args) == 4:
            return [(moved, parse_duration(args[3])), (s, 0.0)]
        return [(moved, 0.0)]
    if cmd == "reset":
        return [(State(keys=set(s.keys)), 0.0)]
    if cmd == "key":
        if len(args) != 2 or args[0].lower() not in ("hold", "release"):
            raise PadError("key hold|release <key|all>")
        if args[1].lower() == "all" and args[0].lower() == "release":
            s.keys.clear()
        elif args[0].lower() == "hold":
            s.keys.add(key_code(args[1]))
        else:
            s.keys.discard(key_code(args[1]))
        return [(s, 0.0)]
    raise PadError(f"unknown command {cmd!r}")


def plugins_dir(slot: int) -> Path:
    """<slots_dir>/<slot_dir_template>/d2rloader/plugins from MODKIT_TESTING_CONFIG (the toolkit's config)."""
    import tomllib
    cfg_path = os.environ.get("MODKIT_TESTING_CONFIG")
    if not cfg_path:
        raise PadError("set MODKIT_TESTING_CONFIG or pass --plugins DIR")
    cfg = tomllib.loads(Path(cfg_path).read_text(encoding="utf-8-sig"))["game"]
    folder = cfg.get("slot_dirs", {}).get(str(slot)) or str(Path(cfg["slots_dir"]) / cfg.get("slot_dir_template", "slot-{slot}").format(slot=slot))
    return Path(folder) / "d2rloader" / "plugins"


def read_state(path: Path) -> State:
    try:
        return State.parse(path.read_text(encoding="ascii"))
    except FileNotFoundError:
        return State()


def write_state(path: Path, s: State) -> None:
    tmp = path.with_name(path.name + ".tmp")
    tmp.write_text(s.line(), encoding="ascii")
    os.replace(tmp, path)  # the plugin opens the file with delete sharing, so the swap is atomic for it


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--slot", type=int, help="game slot number (config from MODKIT_TESTING_CONFIG)")
    ap.add_argument("--plugins", type=Path, help="the folder that holds test-harness.dll")
    ap.add_argument("command", nargs=argparse.REMAINDER)
    a = ap.parse_args(argv)
    try:
        folder = a.plugins or (plugins_dir(a.slot) if a.slot else None)
        if folder is None:
            raise PadError("give --slot N or --plugins DIR")
        path = folder / PAD_FILE
        cmd = a.command[:1]
        if cmd == ["off"]:
            path.unlink(missing_ok=True)
            print("pad off (state file removed)")
            return 0
        if cmd == ["status"]:
            print(path.read_text(encoding="ascii").strip() if path.exists() else "pad off")
            return 0
        for state, wait in plan(a.command, read_state(path)):
            write_state(path, state)
            if wait:
                time.sleep(wait)
        print(read_state(path).line().strip())
        return 0
    except PadError as e:
        print(f"pad: {e}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
