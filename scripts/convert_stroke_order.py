#!/usr/bin/env python3
"""CLI for offline stroke_order.bin conversion. Never accesses the network."""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from stroke_order.convert import main  # noqa: E402


if __name__ == "__main__":
    sys.exit(main())
