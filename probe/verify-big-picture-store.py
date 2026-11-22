"""Check the signed release and whole-output evidence of a public Store visit."""

import argparse
import json
from pathlib import Path
import re
import sys
import uuid

from big_picture_evidence import capture_time, check_png, check_release, load, require


def release_launch(path, first):
    if path.suffix != ".jsonl":
        return load(path)
    boot, *samples = [json.loads(line) for line in path.read_bytes().splitlines() if line]
    require(boot["event"] == "cold-activation" and
            all(image.lower() == "runtimebroker.exe" for image in boot["processes"]),
            "Cold activation requires an empty host, renderer and game baseline")
    require(any(row.get("foreground_status") == "gamepad-ready" and
                row["run_id"] == first["run_id"] and row["host_pid"] == first["host_pid"] and
                boot["timestamp"] < row["timestamp"] <= first["captured_at"] for row in samples),
            "Cold-start log did not reach the captured live host")
    return {"version": boot["version"], "package": boot.get("package") or first["package"],
            "timestamp_before_launch": boot["timestamp"], "baseline_run_id": boot["previous"]}


def is_key(action, name, vk):
    return (action["action"] == "WDP keyboard " + name or
            (action["action"] == "WDP keyboard QA" and action.get("vk") == vk))


def verify(appx, release_pin, launch_path, directory, phase_pins):
    route = ["library-before-store", "store-menu", "store-landing"]
    if "store-product" in phase_pins:
        route.append("store-product")
    route.append("library-after-store")
    require(set(phase_pins) == set(route + ["internet-settings"]),
            "Missing or unexpected phase pins for the Store and Internet settings checks")
    first = load((directory / Path(phase_pins[route[0]]["file"]).name).with_suffix(".json"))
    launch = release_launch(launch_path, first)
    check_release(appx, release_pin, launch)
    captures = {}
    hashes = set()
    for phase, pin in phase_pins.items():
        filename = pin["file"]
        require(Path(filename).name == filename and re.fullmatch(re.escape(phase) + r"-\d+\.png", filename),
                f"Unexpected capture filename for {phase}")
        path = (directory / filename).with_suffix(".json")
        receipt = load(path)
        require(receipt["file"] == filename and receipt["phase"] == phase and receipt["source"] == "official TV capture" and
                receipt["host_live"] is True and receipt["package_version"] == launch["version"] and
                receipt["sha256"] == pin["sha256"], f"Capture source, version or digest differs for {phase}")
        require(launch["timestamp_before_launch"] < receipt["captured_at"] <= capture_time(path) and
                receipt["sha256"] not in hashes, f"Stale or reused capture for {phase}")
        if launch_path.suffix == ".jsonl":
            require(receipt.get("package") == launch["package"], f"Installed package changed at {phase}")
        hashes.add(receipt["sha256"])
        check_png(path, pin["sha256"])
        captures[phase] = receipt
    first = captures[route[0]]
    run = str(uuid.UUID(first["run_id"]))
    pid = first["host_pid"]
    require(type(pid) is int and pid > 0 and run != launch["baseline_run_id"], "Expected a fresh native host")
    for phase, receipt in captures.items():
        require(receipt["host_pid"] == pid and receipt["run_id"] == run, f"Host changed at {phase}")
        for sample in (receipt["process_before"], receipt["processes"]):
            owners = [proc["pid"] for proc in sample if proc["image"].lower() == "steamprimary.exe"]
            require(owners == [pid], f"Actual production host process missing at {phase}")
    for earlier, later in zip(route, route[1:]):
        require(capture_time(directory / captures[earlier]["file"]) < captures[later]["captured_at"],
                f"Screenshot order changed between {earlier} and {later}")
    require(first["public_store_navigation"] == "" and captures["store-menu"]["public_store_navigation"] == "",
            "A previous Store navigation is present; this first-visit check needs a fresh sequence")
    for phase in ("store-landing", "store-product"):
        if phase in captures:
            require(re.fullmatch(r"loaded http=2\d\d", captures[phase]["public_store_navigation"]),
                    f"The external Store had no successful 2xx navigation at {phase}")
    settings = captures["internet-settings"]
    net = settings["xbox_network"]
    require(re.fullmatch(r"internet (wifi|ethernet)", net) and settings["native_network_before"] == net,
            "The preferred active Wi-Fi or Ethernet route was not stable across the Internet settings capture")
    actions = [json.loads(line) for line in (directory / "input.jsonl").read_bytes().splitlines() if line]
    begin, end = first["captured_at"], captures["library-after-store"]["captured_at"]
    within = [a for a in actions if begin < a["time"] < end]
    require(all(a["run_id"] == run and a["host_pid"] == pid and a["action"].startswith("WDP keyboard ") for a in within),
            "Different native host or a non-keyboard operator action during this sequence")
    selection = captures["store-menu"]
    require(any(is_key(a, "menu", 112) and a["time"] < selection["captured_at"] for a in within) and
            any(is_key(a, "accept", 13) and capture_time(directory / selection["file"]) < a["time"] <
                captures["store-landing"]["captured_at"] for a in within), "Missing WDP menu or Store selection evidence")
    print(f"Verified retained TV receipts for {launch['version']} run {run}, SteamPrimary PID {pid}, {net}.")
    print(f"Native Store result at landing: {captures['store-landing']['public_store_navigation']}.")
    for phase in route + ["internet-settings"]:
        print(f"{phase}: {captures[phase]['file']} {captures[phase]['sha256']}")
    print("Inspect the original frames for the selected Store row, storefront, product when present, Library and Settings.")
    print("The operator log establishes keyboard events. Controller input and physical cursor visibility require separate evidence.")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appx", type=Path)
    parser.add_argument("launch", type=Path)
    parser.add_argument("capture_directory", type=Path)
    parser.add_argument("phase_pins", type=Path, help="independently retained JSON mapping each phase to file and sha256")
    parser.add_argument("--sha256", required=True, help="independent signed release SHA-256")
    args = parser.parse_args()
    try:
        verify(args.appx, args.sha256, args.launch, args.capture_directory, load(args.phase_pins))
    except (ValueError, KeyError, TypeError, OSError) as error:
        sys.exit(f"Evidence check failed: {error}")
