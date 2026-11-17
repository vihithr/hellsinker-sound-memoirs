"""Prepare browser art and track order from the desktop originals (no game audio)."""
import json
from pathlib import Path
import re
from PIL import Image
root = Path(__file__).resolve().parents[1]
out = root / "wasm/assets"
out.mkdir(parents=True, exist_ok=True)
for target, source in {"cells":"themed/cells_0", "title":"memoirs-title", "axis":"memoirs-axis"}.items():
    Image.open(root / f"assets/{source}.bmp").save(out / f"{target}.png", optimize=True)
names = re.findall(r'"([\w]+)"', (root/"src/core/game_tracks.h").read_text())
(out / "tracks.json").write_text(json.dumps(names) + "\n")
