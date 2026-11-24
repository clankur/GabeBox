"""Compatibility entry point for the account-free primary control."""

from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from build_tools.prepare_primary_control import main

if __name__ == "__main__":
    main()
