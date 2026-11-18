"""Recolor the original title artwork per theme.

The 418x56 memoirs-title.bmp carries three source color families:
  (223,108,70)  -> theme.title       (stroke fill)
  (204,204,204) -> theme.titleBlock  (block letterform)
  everything else is backdrop/antialiasing near the ORIGINAL outer
  (217,227,224) -> theme.outer.

Each pixel is classified by nearest source color (same hard-nearest
approach as make_themed_art.py) and remapped to the theme counterpart.
Outputs assets/themed/title_<i>.bmp for the desktop rc and
wasm/assets/title_<i>.png for the browser atlas.
"""
import json
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / "assets/memoirs-title.bmp"

with open(ROOT / "assets/palettes.json", encoding="utf8") as f:
    palettes = json.load(f)

def rgb(hexstr):
    return tuple(int(hexstr[i:i+2], 16) for i in (0, 2, 4))

SOURCES = [(223, 108, 70), (204, 204, 204), (217, 227, 224)]  # title/block/outer

def nearest_target(pixel, theme):
    bi, bd = 0, sum((a - b) ** 2 for a, b in zip(pixel, SOURCES[0]))
    for i, src in enumerate(SOURCES[1:], 1):
        d = sum((a - b) ** 2 for a, b in zip(pixel, src))
        if d < bd:
            bd, bi = d, i
    keys = ("title", "titleBlock", "outer")
    return theme[keys[bi]]  # values are already RGB tuples

src = Image.open(SRC).convert("RGB")
for index, (name, spec) in enumerate(palettes.items()):
    theme = {k: rgb(v) for k, v in spec.items()}
    im = src.copy()
    px = im.load()
    w, h = im.size
    for y in range(h):
        for x in range(w):
            px[x, y] = nearest_target(px[x, y], theme)
    out_dir = ROOT / "assets/themed"
    out_dir.mkdir(exist_ok=True)
    im.save(out_dir / f"title_{index}.bmp")
    wasm = ROOT / "wasm/assets"
    wasm.mkdir(exist_ok=True)
    im.save(wasm / f"title_{index}.png", optimize=True)
    print(f"title_{index} ({name}) written")
