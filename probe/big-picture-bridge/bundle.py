"""Assemble the Xbox adapters with the URL definitions from the pinned Steam release."""
import argparse
import hashlib
import json
from pathlib import Path
import re


SOURCE = Path(__file__).resolve().parent
URL_LIST_SHA256 = "3958c79099dc94ee99c3a5dc841cb70530013c68656649e5c96fc931c5c71263"
MODULES = (
    "xbox-gamepads.js", "xbox-controller-service.js", "xbox-notifications.js", "xbox-game-actions.js", "xbox-user.js", "xbox-gamepad-surface.js", "xbox-window.js", "xbox-browser-view.js", "xbox-store-browser.js", "xbox-input.js",
    "xbox-lifecycle.js", "xbox-network.js", "xbox-messaging.js", "xbox-core-startup.js", "xbox-services.js",
    "xbox-cloud-cache.js", "xbox-settings.js", "xbox-url-roots.js", "xbox-url.js", "bridge.js",
)


def bundle(url_list):
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
    return "\n".join(["(() => {", "'use strict';", version, definitions,
                      *((SOURCE / name).read_text() for name in MODULES), "})();", ""])


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("url_list", type=Path)
    print(bundle(parser.parse_args().url_list), end="")
