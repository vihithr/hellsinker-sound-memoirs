"""Replay a WASM scene command dump and diff it against the desktop --shot BMP.

Usage (from player/):
  node wasm/core/dump_scene.mjs obj/scene_dump.bin
  dist\\hsbgm.exe --shot obj/baseline_theme0.bmp --theme 0 --stage 0 --page 0 --shot-scale 1
  python tools/verify_scene_pixels.py
"""
import struct
import sys
from pathlib import Path
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[1]
baseline_name = sys.argv[1] if len(sys.argv) > 1 else "baseline_theme0.bmp"
data = (root / "obj/scene_dump.bin").read_bytes()
off = 0
n = struct.unpack_from("<I", data, off)[0]; off += 4
cmds = data[off:off + n * 20]; off += n * 20
mp = struct.unpack_from("<I", data, off)[0]; off += 4
pts = data[off:off + mp * 4]; off += mp * 4
# Optional text pool tail (SYS/LIB pages). Absent in old dumps.
texts = []
if off + 4 <= len(data):
    tc = struct.unpack_from("<I", data, off)[0]; off += 4
    texts_raw = data[off:off + tc * 20]; off += tc * 20
    if off + 4 <= len(data):
        tpc = struct.unpack_from("<I", data, off)[0]; off += 4
        text_pts = data[off:off + tpc * 4]
    else:
        text_pts = b''
    for i in range(tc):
        cpo, cnt = struct.unpack_from("<HH", texts_raw, i * 20)
        x, y, right, bottom = struct.unpack_from("<hhhh", texts_raw, i * 20 + 4)
        align = texts_raw[i * 20 + 12]
        color = struct.unpack_from("<I", texts_raw, i * 20 + 16)[0]
        codes = struct.unpack_from("<%dI" % cnt, text_pts, cpo * 4) if cnt else ()
        texts.append((codes, x, y, right, bottom, align, color))

img = Image.new("RGB", (640, 480), (0, 0, 0))
pix = img.load()
# Optional env overrides for reviewing non-baseline frames:
#   HS_DUMP=<bin path>  HS_THEME=<0..4>  HS_OUT=<png path>
import os
if os.environ.get("HS_DUMP"):
    data = Path(os.environ["HS_DUMP"]).read_bytes()
    off = 0
    n = struct.unpack_from("<I", data, off)[0]; off += 4
    cmds = data[off:off + n * 20]; off += n * 20
    mp = struct.unpack_from("<I", data, off)[0]; off += 4
    pts = data[off:off + mp * 4]; off += mp * 4
    texts = []
theme_idx = int(os.environ.get("HS_THEME", "0"))
cells = Image.open(root / f"wasm/assets/cells_{theme_idx}.png").convert("RGB")
# Full artwork addressed by src >= kGfxImageBase (100): 100..104 = themed
# title_0..4.png (recolored per theme by make_themed_title.py).
images = {
    100 + i: Image.open(root / f"wasm/assets/title_{i}.png").convert("RGB")
    for i in range(5)
}


def cmd(i):
    b = i * 20
    op, src, width, alpha = cmds[b], cmds[b + 1], cmds[b + 2], cmds[b + 3]
    color = struct.unpack_from("<I", cmds, b + 4)[0]
    x0, y0, x1, y1 = struct.unpack_from("<hhhh", cmds, b + 8)
    po, pc = struct.unpack_from("<HH", cmds, b + 16)
    return op, src, width, alpha, (color & 255, (color >> 8) & 255, (color >> 16) & 255), x0, y0, x1, y1, po, pc


def pt(i):
    return struct.unpack_from("<hh", pts, i * 4)


clip_stack = [(0, 0, 640, 480)]


def rect(x0, y0, x1, y1, color):
    cx0, cy0, cx1, cy1 = clip_stack[-1]
    for y in range(max(0, y0, cy0), min(480, y1, cy1)):
        for x in range(max(0, x0, cx0), min(640, x1, cx1)):
            pix[x, y] = color


def alpha_rect(x0, y0, x1, y1, color, alpha):
    a = alpha / 255.0
    for y in range(max(0, y0), min(480, y1)):
        for x in range(max(0, x0), min(640, x1)):
            r, g, b = pix[x, y]
            pix[x, y] = (int(r * (1 - a) + color[0] * a + .5),
                         int(g * (1 - a) + color[1] * a + .5),
                         int(b * (1 - a) + color[2] * a + .5))


def alpha_ellipse(x0, y0, x1, y1, color, alpha):
    import math
    cx, cy = (x0 + x1) / 2.0, (y0 + y1) / 2.0
    rx, ry = max(1.0, (x1 - x0) / 2.0), max(1.0, (y1 - y0) / 2.0)
    a0 = alpha / 255.0
    for y in range(max(0, y0), min(480, y1)):
        for x in range(max(0, x0), min(640, x1)):
            dx, dy = (x + .5 - cx) / rx, (y + .5 - cy) / ry
            d = math.sqrt(dx * dx + dy * dy)
            if d >= 1.0:
                continue
            a = (1.0 - d) ** 2 * a0
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
    elif op == 11:   # ALPHA_ELLIPSE
        alpha_ellipse(x0, y0, x1, y1, color, alpha)
    elif op == 4:    # LINE
        line(x0, y0, x1, y1, color, width)
    elif op == 5:    # POLYLINE
        ax, ay = pt(po)
        for k in range(1, pc):
            bx, by = pt(po + k)
            line(ax, ay, bx, by, color, width)
            ax, ay = bx, by
    elif op == 9:    # PUSH_CLIP
        cx0, cy0, cx1, cy1 = clip_stack[-1]
        clip_stack.append((max(cx0, x0), max(cy0, y0), min(cx1, x1), min(cy1, y1)))
    elif op == 10:   # POP_CLIP
        if len(clip_stack) > 1:
            clip_stack.pop()
    elif op == 8:    # BLIT
        # Source rect in x0,y0,x1,y1; destination pos in point[0],
        # destination size in point[1] ({0,0} falls back to source size).
        dx, dy = pt(po)
        dw, dh = pt(po + 1)
        dw = dw or x1; dh = dh or y1
        source = images.get(src, cells)
        tile = source.crop((x0, y0, x0 + x1, y0 + y1))
        if (dw, dh) != (x1, y1):
            tile = tile.resize((dw, dh), Image.NEAREST)
        img.paste(tile, (dx, dy))

# Render the text pool with Fusion Pixel 12px (matches the desktop GDI font).
if texts:
    from PIL import ImageFont
    font_path = root / "assets/fonts/fusion-pixel-12px-monospaced-zh_hans.ttf"
    font = ImageFont.truetype(str(font_path), 12) if font_path.exists() else None
    td = ImageDraw.Draw(img)
    for codes, x, y, right, bottom, align, color in texts:
        s = "".join(chr(c) for c in codes if c)
        if not s or not font:
            continue
        rgb = (color & 255, (color >> 8) & 255, (color >> 16) & 255)
        try:
            bb = td.textbbox((0, 0), s, font=font)
            tw, th = bb[2] - bb[0], bb[3] - bb[1]
        except Exception:
            tw, th = len(s) * 12, 12
        tx = x if align == 0 else ((x + right - tw) // 2 if align == 1 else right - tw)
        ty = y + (bottom - y - th) // 2
        td.text((tx, ty), s, fill=rgb, font=font)

out_path = Path(os.environ["HS_OUT"]) if os.environ.get("HS_OUT") else root / "obj/scene_render.png"
img.save(out_path)
if os.environ.get("HS_DUMP"):
    print("rendered:", out_path)
    sys.exit(0)

ref = Image.open(root / "obj" / baseline_name).convert("RGB")
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
