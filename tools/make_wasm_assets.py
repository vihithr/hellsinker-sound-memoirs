"""Prepare browser art and track order from the desktop originals (no game audio)."""
import json
from pathlib import Path
import re
from PIL import Image
root = Path(__file__).resolve().parents[1]
out = root / "wasm/assets"
out.mkdir(parents=True, exist_ok=True)

# Five pre-rendered 8x11 cell atlases, one per desktop kThemes entry.
# The derived painter emits a blit command carrying the active theme index.
for i in range(5):
    src = root / f"assets/themed/cells_{i}.bmp"
    if src.exists():
        Image.open(src).save(out / f"cells_{i}.png", optimize=True)

# Backwards-compatible single atlas (ORIGINAL) and legacy title/axis art.
for source, target in [("themed/cells_0", "cells"),
                       ("memoirs-title", "title"),
                       ("memoirs-axis", "axis")]:
    src = root / f"assets/{source}.bmp"
    if src.exists():
        Image.open(src).save(out / f"{target}.png", optimize=True)

names = re.findall(r'"([\w]+)"', (root / "src/core/game_tracks.h").read_text())
(out / "tracks.json").write_text(json.dumps(names) + "\n")
