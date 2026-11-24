#!/usr/bin/env python3
"""Compatibility entry point for tools/verify-big-picture-display.py."""

from pathlib import Path
from runpy import run_path
import sys

if __name__ == "__main__":
    script = Path(__file__).resolve().parents[1] / "tools/verify-big-picture-display.py"
    sys.path.insert(0, str(script.parent))
    run_path(str(script), run_name="__main__")
