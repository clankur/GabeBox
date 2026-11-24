#!/usr/bin/env python3
"""Compatibility entry point for tools/steam_compatibility.py."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from tools.steam_compatibility import main

if __name__ == "__main__":
    main()
