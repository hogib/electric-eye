#!/usr/bin/env python3
"""Tests for tools/eeye-record, the topside encoder.

The parts worth testing are the ones that decide what ends up in the file
and whether it can be played back: duration parsing, the ffmpeg command
that gets built, and the description shown to the operator. Actually
running ffmpeg is covered by end-to-end testing, not here.
"""
import importlib.util
import os
import sys
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_SCRIPT = os.path.join(_HERE, "..", "tools", "eeye-record")

# The tool has no .py extension (it is a command, not a module), so load
# it by path rather than by import.
_spec = importlib.util.spec_from_loader(
    "eeye_record",
    importlib.machinery.SourceFileLoader("eeye_record", _SCRIPT),
)
rec = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(rec)


class Args:
    """Stands in for argparse's namespace."""
    def __init__(self, **kw):
        self.output = "out.mp4"
        self.quality = "medium"
        self.codec = "libx264"
        self.duration = None
        self.split = None
        self.__dict__.update(kw)


class TestDurationParsing(unittest.TestCase):
    """A dive is measured in minutes; making someone convert to seconds is
    a pointless chance to get it wrong."""

    def test_bare_number_is_seconds(self):
        self.assertEqual(rec.parse_duration("90"), 90)
        self.assertEqual(rec.parse_duration(45), 45)

    def test_unit_suffixes(self):
        self.assertEqual(rec.parse_duration("90s"), 90)
        self.assertEqual(rec.parse_duration("15m"), 900)
        self.assertEqual(rec.parse_duration("2h"), 7200)

    def test_fractional_values(self):
        self.assertEqual(rec.parse_duration("1.5m"), 90)
        self.assertEqual(rec.parse_duration("0.5h"), 1800)

    def test_case_and_whitespace_tolerated(self):
        self.assertEqual(rec.parse_duration(" 15M "), 900)

    def test_nonsense_is_rejected(self):
        for bad in ("", "abc", "10x", "m"):
            with self.assertRaises(ValueError):
                rec.parse_duration(bad)


class TestFfmpegCommand(unittest.TestCase):
    """The command decides whether the resulting file is playable and
    honest about timing."""

    def test_uses_wallclock_timestamps(self):
        # The multipart stream carries no timestamps. Without this a hitch
        # in the tether silently shifts the recording out of sync with
        # when things actually happened.
        cmd = rec.build_ffmpeg_command(Args(), "http://x/stream")
        self.assertIn("-use_wallclock_as_timestamps", cmd)

    def test_faststart_so_a_truncated_file_still_plays(self):
        # A recording can end by the boat losing power rather than by
        # someone pressing stop.
        cmd = rec.build_ffmpeg_command(Args(), "http://x/stream")
        self.assertIn("+faststart", cmd)

    def test_widely_playable_pixel_format(self):
        cmd = rec.build_ffmpeg_command(Args(), "http://x/stream")
        self.assertIn("yuv420p", cmd)

    def test_quality_presets_map_to_crf(self):
        for name in ("low", "medium", "high"):
            cmd = rec.build_ffmpeg_command(Args(quality=name), "http://x/s")
            crf = cmd[cmd.index("-crf") + 1]
            self.assertEqual(int(crf), rec.QUALITY_PRESETS[name]["crf"])

    def test_lower_crf_means_higher_quality(self):
        # Guards the direction of the dial: crf is inverted, so a preset
        # table edited carelessly could silently swap low and high.
        self.assertLess(rec.QUALITY_PRESETS["high"]["crf"],
                        rec.QUALITY_PRESETS["medium"]["crf"])
        self.assertLess(rec.QUALITY_PRESETS["medium"]["crf"],
                        rec.QUALITY_PRESETS["low"]["crf"])

    def test_duration_is_passed_through(self):
        cmd = rec.build_ffmpeg_command(Args(duration="10m"), "http://x/s")
        self.assertIn("-t", cmd)
        self.assertEqual(cmd[cmd.index("-t") + 1], "600")

    def test_split_uses_segment_muxer(self):
        cmd = rec.build_ffmpeg_command(Args(split="5m"), "http://x/s")
        self.assertIn("segment", cmd)
        self.assertEqual(cmd[cmd.index("-segment_time") + 1], "300")
        # Timestamps must reset per segment, or every file after the first
        # starts at a nonzero offset and players show a long black lead-in.
        self.assertIn("-reset_timestamps", cmd)

    def test_input_url_is_the_last_positional_before_output(self):
        cmd = rec.build_ffmpeg_command(Args(), "http://host:8080/stream")
        self.assertEqual(cmd[cmd.index("-i") + 1], "http://host:8080/stream")
        self.assertEqual(cmd[-1], "out.mp4")


class TestSourceDescription(unittest.TestCase):
    """What is about to be recorded is decided by two drone-side settings
    that are invisible in the resulting file, so the tool says them out
    loud before it starts."""

    def test_reports_effects_burned_in(self):
        d = rec.describe_source({"stream_frame_interval": 2,
                                 "stream_raw": False, "stream_quality": 75})
        self.assertIn("processed", d)
        self.assertIn("15 fps", d)  # 30 / 2
        self.assertIn("75", d)

    def test_reports_raw_camera(self):
        d = rec.describe_source({"stream_frame_interval": 3,
                                 "stream_raw": True})
        self.assertIn("raw camera", d)
        self.assertIn("10 fps", d)  # 30 / 3

    def test_warns_when_the_tap_is_off(self):
        # stream_frame_interval 0 means no frames are sent at all, so a
        # recording would silently capture nothing -- the single most
        # useful thing to catch before starting.
        d = rec.describe_source({"stream_frame_interval": 0})
        self.assertIn("OFF", d)

    def test_missing_config_is_admitted_not_guessed(self):
        self.assertIn("unknown", rec.describe_source(None))


if __name__ == "__main__":
    unittest.main(verbosity=2)
