import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import pad  # noqa: E402


class PlanTests(unittest.TestCase):
    def test_press_is_down_then_up(self):
        (down, wait), (up, _) = pad.plan(["press", "a"], pad.State())
        self.assertEqual(down.buttons, 0x1000)
        self.assertEqual(wait, 0.1)
        self.assertEqual(up.buttons, 0)

    def test_stick_up_for_500ms(self):
        (moved, wait), (back, _) = pad.plan(["stick", "L", "0.0", "1.0", "500ms"], pad.State())
        self.assertEqual((moved.lx, moved.ly, wait), (0, 32767, 0.5))
        self.assertEqual((back.lx, back.ly), (0, 0))

    def test_hold_keeps_other_state_and_triggers(self):
        s = pad.plan(["hold", "RT"], pad.plan(["hold", "lb"], pad.State())[0][0])[0][0]
        self.assertEqual((s.buttons, s.rt), (0x0100, 255))
        self.assertEqual(pad.plan(["release", "all"], s)[0][0].buttons, 0)

    def test_keys_survive_pad_commands(self):
        s = pad.plan(["key", "hold", "ctrl"], pad.State())[0][0]
        self.assertEqual(s.keys, {0x11})
        self.assertEqual(pad.plan(["press", "A"], s)[1][0].keys, {0x11})
        self.assertEqual(pad.plan(["key", "release", "all"], s)[0][0].keys, set())

    def test_file_line_round_trip(self):
        s = pad.State(buttons=0x9000, lt=255, lx=-32767, ry=5, keys={17, 160})
        self.assertEqual(pad.State.parse(s.line()), s)

    def test_durations(self):
        self.assertEqual(pad.parse_duration("2s"), 2.0)
        self.assertEqual(pad.parse_duration("250ms"), 0.25)

    def test_bad_input_is_refused(self):
        for bad in (["press", "Z"], ["stick", "L", "2", "0"], ["stick", "M", "0", "0"], ["jump"], ["key", "hold", "nope"], []):
            with self.assertRaises(pad.PadError):
                pad.plan(bad, pad.State())


if __name__ == "__main__":
    unittest.main()
