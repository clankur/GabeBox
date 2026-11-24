#!/usr/bin/env python3
"""Compatibility entry point for build_tools/redirect_pe_imports.py."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from build_tools.redirect_pe_imports import main

if __name__ == "__main__":
    main()
