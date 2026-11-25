"""Source paths for checkout-based packaging commands."""

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
STEAM_PINS = ROOT / "runtime/steam/pins"
SIGNIN = ROOT / "ui/signin"
DOS_RUNTIME = ROOT / "runtime/dos"
