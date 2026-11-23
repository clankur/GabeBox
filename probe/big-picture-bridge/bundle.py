"""Assemble the Xbox adapters with the URL definitions from the pinned Steam release."""
import argparse
import hashlib
import json
from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parent
URL_LIST_SHA256 = "3958c79099dc94ee99c3a5dc841cb70530013c68656649e5c96fc931c5c71263"
MODULES = (
    "xbox-gamepads.js", "xbox-controller-service.js", "xbox-notifications.js", "xbox-game-actions.js", "xbox-installs.js", "xbox-downloads.js", "xbox-user.js", "xbox-gamepad-surface.js", "xbox-window.js", "xbox-browser-view.js", "xbox-store-browser.js", "xbox-input.js",
    "xbox-lifecycle.js", "xbox-network.js", "xbox-messaging.js", "xbox-core-startup.js", "xbox-services.js",
    "xbox-cloud-cache.js", "xbox-settings.js", "xbox-storage.js", "xbox-startup-movie.js", "xbox-startup-timing.js",
    "xbox-url-roots.js", "xbox-url.js", "bridge.js",
)


def bundle(url_list, movie_probe_assets=None, profile_startup_services=False, local_cloud_cache_reads=False):
    raw = url_list.read_bytes()
    if hashlib.sha256(raw).hexdigest() != URL_LIST_SHA256:
        raise ValueError("Expected public/url_list.txt from the pinned Steam release.")
    # This release uses a flat KeyValues block per URL. Keep Valve's family
    # filter with its template when handing definitions to the client.
    lines = [line.strip() for line in raw.decode("utf-8-sig").splitlines()
             if line.strip() and not line.lstrip().startswith("//")]
    entries = {}
    it = iter(lines)
    for name in it:
        if not re.fullmatch(r"\w+", name) or name in entries or next(it, None) != "{":
            raise ValueError(f"Unexpected Steam URL definition: {name!r}")
        fields = {}
        for line in it:
            if line == "}":
                break
            field = re.fullmatch(r'(\w+)\s+"([^"]*)"(?:\s*//.*)?', line)
            if not field:
                raise ValueError(f"Unexpected field in {name}: {line!r}")
            fields[field[1].lower()] = field[2]
        else:
            raise ValueError(f"Unterminated Steam URL definition: {name}")
        entries[name] = [fields["url"], fields.get("filter", "")]

    release = json.loads((SOURCE.parents[1] / "analysis/valve-release.json").read_text())
    version = "const xboxSteamReleaseVersion = " + str(int(release["version"])) + ";"
    definitions = "const xboxUrlTemplates = " + json.dumps(entries, separators=(",", ":")) + ";"
    absent_override = False
    if movie_probe_assets is not None:
        movie = Path("movies/bigpicture_startup.webm")
        if not (movie_probe_assets / movie).is_file():
            raise ValueError("Expected the packaged Valve startup movie.")
        try:
            (movie_probe_assets / "uioverrides" / movie).lstat()
        except FileNotFoundError:
            absent_override = True
    movie_probe = "const xboxStartupMovieOverrideAbsent = " + json.dumps(absent_override) + ";"
    profile = "const xboxProfileStartupServices = " + json.dumps(profile_startup_services) + ";"
    cloud_reads = "const xboxLocalCloudCacheReads = " + json.dumps(local_cloud_cache_reads) + ";"
    return "\n".join(["(() => {", "'use strict';", version, definitions, movie_probe, profile, cloud_reads,
                      *((SOURCE / name).read_text() for name in MODULES), "})();", ""])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url_list", type=Path)
    parser.add_argument("--movie-probe-assets", type=Path,
                        help="verify absence of the optional startup movie in this staged Steam UI layout")
    parser.add_argument("--profile-startup-services", action="store_true",
                        help="record bounded timings for the pinned Valve startup-service groups")
    parser.add_argument("--local-cloud-cache-reads", action="store_true",
                        help="trial account-scoped Steam UI cloud cache reads through the pinned Axios bridge")
    args = parser.parse_args()
    print(bundle(args.url_list, args.movie_probe_assets, args.profile_startup_services, args.local_cloud_cache_reads), end="")
