"""Replay a WASM scene command dump and diff it against the desktop --shot BMP.

Usage (from player/):
  node wasm/core/dump_scene.mjs obj/scene_dump.bin
  dist\\hsbgm.exe --shot obj/baseline_theme0.bmp --theme 0 --stage 0 --page 0 --shot-scale 1
  python tools/verify_scene_pixels.py
"""
import struct
from pathlib import Path
from PIL import Image

root = Path(__file__).resolve().parents[1]
data = (root / "obj/scene_dump.bin").read_bytes()
off = 0
n = struct.unpack_from("<I", data, off)[0]; off += 4
cmds = data[off:off + n * 20]; off += n * 20
mp = struct.unpack_from("<I", data, off)[0]; off += 4
pts = data[off:off + mp * 4]

img = Image.new("RGB", (640, 480), (0, 0, 0))
pix = img.load()
cells = Image.open(root / "wasm/assets/cells_0.png").convert("RGB")


def cmd(i):
    b = i * 20
    op, src, width, alpha = cmds[b], cmds[b + 1], cmds[b + 2], cmds[b + 3]
    color = struct.unpack_from("<I", cmds, b + 4)[0]
    x0, y0, x1, y1 = struct.unpack_from("<hhhh", cmds, b + 8)
    po, pc = struct.unpack_from("<HH", cmds, b + 16)
    return op, src, width, alpha, (color & 255, (color >> 8) & 255, (color >> 16) & 255), x0, y0, x1, y1, po, pc


def pt(i):
    return struct.unpack_from("<hh", pts, i * 4)


def rect(x0, y0, x1, y1, color):
    for y in range(max(0, y0), min(480, y1)):
        for x in range(max(0, x0), min(640, x1)):
            pix[x, y] = color


def alpha_rect(x0, y0, x1, y1, color, alpha):
    a = alpha / 255.0
    for y in range(max(0, y0), min(480, y1)):
        for x in range(max(0, x0), min(640, x1)):
            r, g, b = pix[x, y]
            pix[x, y] = (int(r * (1 - a) + color[0] * a + .5),
                         int(g * (1 - a) + color[1] * a + .5),
                         int(b * (1 - a) + color[2] * a + .5))


def line(x0, y0, x1, y1, color, width):
    x0, y0, x1, y1 = int(x0), int(y0), int(x1), int(y1)
    if width == 1 and y0 == y1:
        x1 += -1 if x1 > x0 else 1
    dx, sx = abs(x1 - x0), (1 if x0 < x1 else -1)
    dy, sy = -abs(y1 - y0), (1 if y0 < y1 else -1)
    err = dx + dy
    off = width >> 1
    while True:
        rect(x0 - off, y0 - off, x0 - off + width, y0 - off + width, color)
        if x0 == x1 and y0 == y1:
            break
        e2 = 2 * err
        if e2 >= dy:
            err += dy; x0 += sx
        if e2 <= dx:
            err += dx; y0 += sy


for i in range(n):
    op, src, width, alpha, color, x0, y0, x1, y1, po, pc = cmd(i)
    if op == 1:      # CLEAR
        rect(0, 0, 640, 480, color)
    elif op == 2:    # RECT
        rect(x0, y0, x1, y1, color)
    elif op == 3:    # ALPHA_RECT
        alpha_rect(x0, y0, x1, y1, color, alpha)
    elif op == 4:    # LINE
        line(x0, y0, x1, y1, color, width)
    elif op == 5:    # POLYLINE
        ax, ay = pt(po)
        for k in range(1, pc):
            bx, by = pt(po + k)
            line(ax, ay, bx, by, color, width)
            ax, ay = bx, by
    elif op == 8:    # BLIT
        dx, dy = pt(po)
        tile = cells.crop((x0, y0, x0 + x1, y0 + y1))
        img.paste(tile, (dx, dy))

img.save(root / "obj/scene_render.png")

ref = Image.open(root / "obj/baseline_theme0.bmp").convert("RGB")
rp = ref.load()
total = 0
worst = 0
changed = 0
hist = {}
for y in range(480):
    for x in range(640):
        a = pix[x, y]
        b = rp[x, y]
        d = max(abs(a[0] - b[0]), abs(a[1] - b[1]), abs(a[2] - b[2]))
        total += d
        worst = max(worst, d)
        if d:
            changed += 1
            hist[d] = hist.get(d, 0) + 1
mean = total / (640 * 480)
print(f"commands={n} points={mp}")
print(f"mean_abs_diff={mean:.4f} max_diff={worst} changed_pixels={changed}")
print("top diffs:", sorted(hist.items(), key=lambda kv: -kv[1])[:10])
print("rendered:", root / "obj/scene_render.png")
