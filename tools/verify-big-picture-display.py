"""Verify an original whole-output Xbox Library, Play, overlay and Resume capture sequence."""

import argparse
from bisect import bisect_left
import json
import math
from pathlib import Path
import re
import struct
import sys
import uuid
import xml.etree.ElementTree as ET
import zipfile

from big_picture_evidence import capture_time, check_png, check_release, load, require


PHASES = ("library-before-play", "stock-play", "title", "overlay", "library-while-running", "resume", "resumed-title")


def read_captures(directory, pins):
    require(set(pins) == set(PHASES), "Supply one independently retained PNG digest for each of the seven phases")
    named = {phase: pin if isinstance(pin, dict) else {"sha256": pin} for phase, pin in pins.items()}
    digests = [pin["sha256"] for pin in named.values()]
    require(all(re.fullmatch(r"[0-9a-f]{64}", digest) for digest in digests), "Invalid SHA-256 phase pin")
    require(len(set(digests)) == len(PHASES), "Identical phase PNGs cannot establish distinct screen content")
    captures = {}
    for path in directory.glob("*.json"):
        receipt = load(path)
        phase = receipt.get("phase") if isinstance(receipt, dict) else None
        pin = named.get(phase, {})
        if not pin or receipt.get("sha256") != pin["sha256"] or ("file" in pin and pin["file"] != receipt.get("file")):
            continue
        require(phase not in captures, f"Duplicate receipt for pinned {phase} image")
        require(receipt.get("source") == "official TV capture" and receipt.get("file") == path.with_suffix(".png").name,
                f"{phase} needs an original official TV image with its matching sidecar")
        require(receipt.get("host_live") is True and math.isfinite(receipt["captured_at"]), f"{phase} lacks a live host or timestamp")
        require(abs(capture_time(path) - receipt["captured_at"]) <= 2, f"{phase} filename and capture time disagree")
        check_png(path, pin["sha256"])
        captures[phase] = receipt
    require(set(captures) == set(PHASES), f"Missing pinned TV phases: {', '.join(p for p in PHASES if p not in captures)}")
    timestamps = [captures[phase]["captured_at"] for phase in PHASES]
    require(all(a < b for a, b in zip(timestamps, timestamps[1:])), "TV capture sequence out of order")
    return captures


def tokens(stats):
    return dict(item.split("=", 1) for item in stats.split())


def panel_count(stats, app_id):
    require(stats.get("appid") == str(app_id) and stats.get("primary_panel") == "1" and
            stats.get("primary_media") is None and stats.get("width") == "1920" and stats.get("height") == "1080",
            "Expected the selected game's native 1920x1080 primary panel")
    count = int(stats["presented"])
    gpu, cpu = int(stats["gpu_presented"]), int(stats["cpu_presented"])
    require(gpu >= 0 and cpu >= 0 and count == gpu + cpu, "Invalid native GPU/CPU swap counters")
    return count


def check_process(processes, pid, image):
    return any(proc.get("pid") == pid and proc["image"].casefold() == image.casefold() for proc in processes)


def verify(directory, pins, appx, release, app_id, image, capture_directory=None):
    launch = load(directory / "launch-receipt.json")
    check_release(appx, release, launch)
    shots = read_captures(capture_directory or directory, pins)
    run = str(uuid.UUID(shots["stock-play"]["run_id"]))
    require(run != launch["baseline_run_id"] and launch["timestamp_before_launch"] < shots[PHASES[0]]["captured_at"],
            "The retained captures precede this activation or reuse the prior native run")
    require(all(shot["run_id"] == run for shot in shots.values()), "Run changed between TV captures")
    require(all(shots[phase]["app_id"] == app_id for phase in PHASES[1:]) and
            shots[PHASES[0]]["app_id"] in (None, app_id), "TV receipt refers to a different AppID")
    pid = shots["title"]["matched_pid"]
    require(type(pid) is int and pid > 0 and all(shots[p]["matched_pid"] == pid for p in PHASES[2:]) and
            all(shots[p]["matched_pid"] is None for p in PHASES[:2]), "Game PID does not match the Play sequence")

    rows = [json.loads(line) for line in (directory / "stock-game-events.jsonl").read_bytes().splitlines() if line]
    times = [row["t"] for row in rows]
    require(all(math.isfinite(t) for t in times) and all(a < b for a, b in zip(times, times[1:])),
            "Stock game observations are out of order")
    pairs = {}
    for phase in PHASES:
        shot = shots[phase]
        i = bisect_left(times, shot["captured_at"])
        if phase in PHASES[:2] and not 0 < i < len(rows):
            require("processes" in shot and not check_process(shot["processes"], pid, image),
                    f"Missing pre-Play process evidence at {phase}")
            continue
        require(0 < i < len(rows), f"No time-bracketing process samples for {phase}")
        pair = rows[i - 1:i + 1]
        require(pair[1]["t"] - pair[0]["t"] <= 5 and all(r["run_id"] == run and r["host_live"] is True for r in pair),
                f"No continuous live host around {phase}")
        if phase in PHASES[:2]:
            require(all(not check_process(r["processes"], pid, image) for r in pair), f"Game already running before {phase}")
        else:
            expected = "game-switch-to-steam" if phase in ("overlay", "library-while-running", "resume") else (
                "game-presented" if phase == "title" else "game-switch-to-game")
            require(all(r["status"] == expected for r in pair), f"Unexpected native foreground at {phase}")
        pairs[phase] = pair

    begin = bisect_left(times, shots["title"]["captured_at"]) - 1
    end = bisect_left(times, shots["resumed-title"]["captured_at"])
    window = rows[begin:end + 1]
    require(all(b["t"] - a["t"] <= 5 for a, b in zip(window, window[1:])), "Gap in live game process observations")
    samples = []
    for row in window:
        require(row["run_id"] == run and row["host_live"] is True and row["current_appid"] == app_id and
                row["matched_pid"] == pid and check_process(row["processes"], pid, image),
                "Host, AppID or matched game process changed during the overlay sequence")
        native = row["native"]
        require(not native.get("game_frame_fallback"), "Game fell back from native presentation")
        for field in ("game_frame_first", "game_frame_media", "game_frame_sample", "game_frame_latest_sample"):
            if not native.get(field):
                continue
            stats = tokens(native[field])
            panel_count(stats, app_id)
            if field.endswith("sample") and stats.get("visible") == "1":
                elapsed, frames = int(stats["sample_us"]), int(stats["sample_frames"])
                if elapsed >= 30_000_000 and 0 < frames <= int(stats["presented"]):
                    samples.append((frames, elapsed))
    require(samples, "Missing advancing 30-second foreground native panel sample")
    counters = []
    for phase in ("title", "resumed-title"):
        pair = pairs[phase]
        values = []
        for row in pair:
            stats = tokens(row["native"].get("game_frame_media") or row["native"].get("game_frame_first", ""))
            require(stats.get("visible") == "1", f"Native panel is hidden at {phase}")
            values.append(panel_count(stats, app_id))
        counters.append(min(values))
    require(counters[1] > counters[0] > 0, "Primary-panel swaps did not advance across Resume")
    covered = [r for r in window if r["status"] == "game-switch-to-steam"]
    hidden = [tokens(r["native"]["game_frame_media"]) for r in covered if r["native"].get("game_frame_media")]
    hidden_counts = [panel_count(s, app_id) for s in hidden if s.get("visible") == "0"]
    require(hidden_counts and counters[1] > max(hidden_counts), "Missing actual panel hide and advancing post-Resume swap")
    print(f"Verified raw TV capture receipts for {launch['version']}, run {run}, AppID {app_id}, {image} PID {pid}.")
    print(f"{len(window)} live process samples. Native title/resume swaps {counters[0]} -> {counters[1]}.")
    frames, elapsed = samples[0]
    print(f"First observed qualifying panel sample: {frames} swaps in {elapsed / 1_000_000:.3f} s ({frames * 1_000_000 / elapsed:.2f} FPS).")
    for phase in PHASES:
        print(f"{phase}: {shots[phase]['file']} {shots[phase]['sha256']}")
    print("Independently inspect every image for its actual UI or game. Physical controller input and normal Exit require separate evidence.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appx", type=Path)
    parser.add_argument("run_directory", type=Path)
    parser.add_argument("phase_pins", type=Path, help="independently retained JSON object mapping each phase to its TV PNG SHA-256")
    parser.add_argument("--captures", type=Path, help="cycle directory with the seven PNGs and their capture sidecars")
    parser.add_argument("--sha256", required=True, help="independent signed release SHA-256")
    parser.add_argument("--app-id", required=True, type=int)
    parser.add_argument("--game-image", required=True)
    args = parser.parse_args()
    try:
        verify(args.run_directory, load(args.phase_pins), args.appx, args.sha256, args.app_id, args.game_image, args.captures)
    except (OSError, ValueError, KeyError, TypeError, IndexError, struct.error, ET.ParseError, zipfile.BadZipFile) as error:
        sys.exit(f"Evidence check failed: {error}")


if __name__ == "__main__":
    main()
