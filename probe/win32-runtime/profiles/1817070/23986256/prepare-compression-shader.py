#!/usr/bin/env python3
"""Compatibility entry point for profiles/1817070/23986256/prepare-compression-shader.py."""

from pathlib import Path
from runpy import run_path
import sys

if __name__ == "__main__":
    script = Path(__file__).resolve().parents[5] / "profiles/1817070/23986256/prepare-compression-shader.py"
    sys.path.insert(0, str(script.parent))
    run_path(str(script), run_name="__main__")
