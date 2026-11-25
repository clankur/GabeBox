"""Check one retained Series X Library-while-running and resume observation offline."""

import argparse
from bisect import bisect_left
import json
from pathlib import Path
import struct
import sys
import uuid
import xml.etree.ElementTree as ET
import zipfile

from big_picture_evidence import capture_time, check_png, check_release, load, require


def verify(appx, digest, library_path, game_image, library_pin=None):
    directory = library_path.parent
    launch = load(directory / "launch-receipt.json")
    check_release(appx, digest, launch)
    library = load(library_path)
    require(library_path.name.startswith("stock-library-while-running-") and
            library["source"] == "official TV capture" and library.get("native_status", "game-switch-to-steam") == "game-switch-to-steam",
            "Expected a Library-while-running TV receipt")
    run = str(uuid.UUID(library.get("same_run") or library["run"]))
    require(run != launch["baseline_run_id"], "Run ID was already present before activation")
    library_hash = library.get("sha256") or library_pin
    require(library_hash and (not library_pin or library_pin.lower() == library_hash.lower()),
            "Supply the independently recorded Library PNG hash for a receipt without a digest")
    check_png(library_path, library_hash.lower())
    middle = capture_time(library_path)
    rows = [json.loads(line) for line in (directory / "stock-game-events.jsonl").read_bytes().splitlines() if line]
    times = [row["t"] for row in rows]
    require(all(b > a for a, b in zip(times, times[1:])), "Process observations are out of order")

    def bracket(path, *states):
        index = bisect_left(times, capture_time(path))
        require(0 < index < len(rows), f"Missing bracketing samples for {path.name}")
        pair = rows[index - 1:index + 1]
        require(all(row["status"] in states for row in pair), f"Unexpected native state at {path.name}")
        return pair

    mid_rows = bracket(library_path, "game-switch-to-steam")
    app_id = mid_rows[0]["current_appid"]
    pid = mid_rows[0]["matched_pid"]
    require(type(pid) is int and pid > 0 and type(app_id) is int and app_id > 0 and
            library.get("active_appid", app_id) == app_id and library.get("active_pid", pid) == pid,
            "Library receipt differs from the observed game identity")
    media = sorted((path for path in directory.glob(f"media-element-{app_id}-*.json")
                    if load(path)["run_id"] == run), key=capture_time)
    before = [path for path in media if capture_time(path) < middle]
    after = [path for path in media if capture_time(path) > middle]
    require(before and after, "Need a foreground video receipt on both sides of the Library in the same run")
    first, last = before[-1], after[0]
    require(launch["timestamp_before_launch"] < capture_time(first), "Video precedes this activation")
    start = bisect_left(times, capture_time(first)) - 1
    stop = bisect_left(times, capture_time(last)) + 1
    require(start >= 0 and stop <= len(rows), "Missing observation window")
    window = rows[start:stop]
    require(len(window) >= 2, "No process samples across the three captures")
    max_gap = max(b["t"] - a["t"] for a, b in zip(window, window[1:]))
    # The collector polls about once per second. A long unobserved gap cannot establish continuity.
    require(max_gap <= 5, f"Process sampling gap of {max_gap:.1f} s")
    for row in window:
        require(row["run_id"] == run and row["host_live"] is True and row["matched_pid"] == pid and
                row["current_appid"] == app_id and any(proc["pid"] == pid and proc["image"].casefold() == game_image.casefold()
                for proc in row["processes"]), f"Run, AppID or process identity changed at {row['t']}")

    counters = []
    for path, states in [(first, ("game-presented", "game-switch-to-game")), (last, ("game-switch-to-game",))]:
        pair = bracket(path, *states)
        receipt = load(path)
        require(receipt["source"] == "foreground HTML video element" and receipt["run_id"] == run,
                f"Video receipt has a different run or source at {path.name}")
        check_png(path, receipt["sha256"])
        raw = receipt["media_receipt"]
        require(any(row["native"].get("game_frame_media_capture") == raw for row in pair),
                f"No matching live native capture receipt at {path.name}")
        fields = dict(token.split("=", 1) for token in raw.split())
        require(all(fields.get(k) == v for k, v in {"appid": str(app_id), "capture_saved": "1", "width": "1920",
                "height": "1080", "visible": "1", "primary_media": "1"}.items()), f"Invalid media capture at {path.name}")
        counters.append((int(fields["video_ms"]), int(fields["presented"])))
    require(all(b > a > 0 for a, b in zip(*counters)), "Foreground media time and presented count must advance")
    print(f"Verified {launch['version']} run {run}, AppID {app_id}, PID {pid} ({game_image}).")
    print(f"{len(window)} process samples. Longest gap {max_gap:.2f} s. Library PNG SHA-256 {library_hash}.")
    print(f"Video time {counters[0][0]} → {counters[1][0]} ms. Presented count {counters[0][1]} → {counters[1][1]}.")
    print("Inspect the images for Library and title content. Physical input, TV game output and normal exit require separate evidence.")


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("appx", type=Path)
    cli.add_argument("library_receipt", type=Path, help="stock-library-while-running-<time>.json in the retained run directory")
    cli.add_argument("--sha256", required=True, help="independently recorded signed APPX SHA-256")
    cli.add_argument("--library-sha256", help="independently recorded full-HD TV PNG hash for a sparse collector receipt")
    cli.add_argument("--game-image", required=True, help="expected executable name in the sampled process list")
    args = cli.parse_args()
    try:
        verify(args.appx, args.sha256, args.library_receipt, args.game_image, args.library_sha256)
    except (OSError, ValueError, KeyError, TypeError, IndexError, struct.error, ET.ParseError, zipfile.BadZipFile) as error:
        sys.exit(f"Evidence check failed: {error}")


if __name__ == "__main__":
    main()
