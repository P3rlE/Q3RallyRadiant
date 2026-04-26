#!/usr/bin/env python3
"""
Converts install/bitmaps/splash.svg → install/bitmaps/splash.png  (512×512)
Run once from the repo root:   python3 tools/make_splash.py
Requires cairosvg:             pip install cairosvg
"""

import sys
from pathlib import Path

try:
    import cairosvg
except ImportError:
    sys.exit("cairosvg not found. Run: pip install cairosvg")

SRC  = Path("install/bitmaps/splash.svg")
OUTS = [
    Path("install/bitmaps/splash.png"),
    Path("install/settings/Q3RallyRadiant-1.2026.1/bitmaps_light/splash.png"),
]

if not SRC.exists():
    sys.exit(f"Source not found: {SRC}")

for out in OUTS:
    out.parent.mkdir(parents=True, exist_ok=True)
    cairosvg.svg2png(url=str(SRC), write_to=str(out), output_width=512, output_height=512)
    print(f"  Written: {out}")

print("Done.")
