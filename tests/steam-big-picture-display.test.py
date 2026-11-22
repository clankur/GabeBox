"""Exercise the evidence checker with labeled synthetic PNGs and synthetic process samples."""

import contextlib
import hashlib
import io
import json
from pathlib import Path
import runpy
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

PROBE = Path(__file__).resolve().parents[1] / "probe"
sys.path.insert(0, str(PROBE))
tool = runpy.run_path(str(PROBE / "verify-big-picture-display.py"))
PHASES = tool["PHASES"]
RUN = "00000000-0000-4000-8000-000000000033"
APP = 42
PID = 123
IMAGE = "fixture.exe"


def png(index):
    def chunk(name, data):
        return struct.pack(">I", len(data)) + name + data + struct.pack(">I", zlib.crc32(name + data))
    body = zlib.compress((b"\x00" + bytes([index]) * 1920) * 1080)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", 1920, 1080, 8, 0, 0, 0, 0)) +
            chunk(b"IDAT", body) + chunk(b"IEND", b""))


class DisplayEvidence(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.directory = Path(self.tmp.name)
        self.pins = {}
        self.captures = {}
        launch = {"version": "fixture", "timestamp_before_launch": 999, "baseline_run_id": ""}
        (self.directory / "launch-receipt.json").write_text(json.dumps(launch))
        for i, (phase, when) in enumerate(zip(PHASES, (1001.5, 1002.5, 1040.5, 1050.5, 1053.5, 1057.5, 1070.5))):
            path = self.directory / f"physical-tv-{int(when * 1e9)}.png"
            data = png(i)
            path.write_bytes(data)
            self.pins[phase] = hashlib.sha256(data).hexdigest()
            self.captures[phase] = {"phase": phase, "source": "official TV capture", "captured_at": when,
                                    "file": path.name, "sha256": self.pins[phase], "run_id": RUN, "app_id": APP,
                                    "host_live": True, "matched_pid": None if i < 2 else PID}
        self.rows = []
        for t in range(1000, 1075):
            game = t >= 1005
            covered = 1045 <= t < 1060
            status = ("game-switch-to-steam" if covered else "game-switch-to-game" if t >= 1060 else
                      "game-presented" if game else "gamepad-ready")
            count = min(t - 1005, 40) * 30 + max(0, t - 1060) * 30
            panel = (f"appid={APP} primary_panel=1 width=1920 height=1080 visible={int(not covered)} "
                     f"presented={count} gpu_presented=0 cpu_presented={count}")
            native = {"game_frame_media": panel} if game else {}
            if t >= 1039:
                native["game_frame_sample"] = (f"appid={APP} primary_panel=1 width=1920 height=1080 visible=1 "
                                               "presented=1000 gpu_presented=0 cpu_presented=1000 sample_us=30000211 sample_frames=950")
            self.rows.append({"t": t, "status": status, "host_live": True, "run_id": RUN,
                              "current_appid": APP if game else None, "matched_pid": PID if game else None,
                              "processes": [{"pid": PID, "image": IMAGE}] if game else [], "native": native})

    def verify(self):
        for record in self.captures.values():
            (self.directory / record["file"]).with_suffix(".json").write_text(json.dumps(record))
        (self.directory / "stock-game-events.jsonl").write_text("\n".join(map(json.dumps, self.rows)))
        # The existing checker exercises release identity against retained signed APPX files.
        # These unit cases exercise capture, process and native panel evidence exclusively.
        verify = tool["verify"]
        with patch.dict(verify.__globals__, {"check_release": lambda *args: None}), contextlib.redirect_stdout(io.StringIO()):
            verify(self.directory, self.pins, None, "", APP, IMAGE)

    def test_direct_display_sequence(self):
        self.verify()

    def test_swapped_image(self):
        (self.directory / self.captures["overlay"]["file"]).write_bytes((self.directory / self.captures["stock-play"]["file"]).read_bytes())
        with self.assertRaisesRegex(ValueError, "digest mismatch"):
            self.verify()

    def test_stale_receipt(self):
        self.captures["resumed-title"]["captured_at"] = 1040.5
        with self.assertRaisesRegex(ValueError, "filename and capture time disagree"):
            self.verify()

    def test_choose_exact_acquisition_of_identical_phase(self):
        original = self.captures["title"]
        repeated = dict(original, captured_at=1041.5, file="physical-tv-1041500000000.png")
        path = self.directory / repeated["file"]
        path.write_bytes((self.directory / original["file"]).read_bytes())
        path.with_suffix(".json").write_text(json.dumps(repeated))
        with self.assertRaisesRegex(ValueError, "Duplicate receipt"):
            self.verify()
        self.pins["title"] = {"sha256": original["sha256"], "file": repeated["file"]}
        self.verify()

    def test_same_pixels_in_two_phases(self):
        self.pins["resumed-title"] = self.pins["title"]
        with self.assertRaisesRegex(ValueError, "Identical phase"):
            self.verify()

    def test_wrong_run(self):
        self.rows[56]["run_id"] = "previous-run"
        with self.assertRaisesRegex(ValueError, "changed during"):
            self.verify()

    def test_missing_live_pid(self):
        self.rows[56]["processes"] = [{"pid": PID + 1, "image": IMAGE}]
        with self.assertRaisesRegex(ValueError, "changed during"):
            self.verify()

    def test_video_fallback(self):
        self.rows[56]["native"]["game_frame_media"] = self.rows[56]["native"]["game_frame_media"].replace("primary_panel", "primary_media")
        with self.assertRaisesRegex(ValueError, "native 1920x1080 primary panel"):
            self.verify()

    def test_unverified_tv_source(self):
        self.captures["title"]["source"] = "foreground HTML video element"
        with self.assertRaisesRegex(ValueError, "original official TV"):
            self.verify()

    def test_frozen_resume(self):
        for row in self.rows[60:]:
            row["native"]["game_frame_media"] = self.rows[55]["native"]["game_frame_media"].replace("visible=0", "visible=1")
        with self.assertRaisesRegex(ValueError, "advancing post-Resume"):
            self.verify()


if __name__ == "__main__":
    unittest.main()
