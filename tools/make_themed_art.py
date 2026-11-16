"""Pre-render per-theme bitmap art (cells / title / axis) from the original
crops so the runtime only does LoadBitmap + nearest-neighbor stretch (no DIB
byte-order pitfalls).  Output: assets/themed/{cells,title,axis}_<i>.bmp
"""
import os
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
A = os.path.join(ROOT, "assets")
OUT = os.path.join(A, "themed")
os.makedirs(OUT, exist_ok=True)

def rgb(t): return t
def lerp(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))
def dist(a, b):
    return sum((a[i] - b[i]) ** 2 for i in range(3))
def nearest(c, srcs):
    bi, bd = 0, dist(c, srcs[0])
    for i, s in enumerate(srcs[1:], 1):
        d = dist(c, s)
        if d < bd: bd, bi = d, i
    return bi

THEMES = [
    # outer, panel, ink, digit, underline, title, titleBlock, axisLabel, frame, selBand
    dict(outer=(217,227,224), panel=(216,220,208), ink=(39,39,39), digit=(25,54,170),
         underline=(79,210,185), title=(232,63,8), titleBlock=(205,205,205),
         axisLabel=(232,120,40), frame=(132,230,224), selBand=(222,200,179)),
    dict(outer=(16,18,22), panel=(24,28,34), ink=(220,225,230), digit=(120,180,255),
         underline=(60,220,210), title=(255,120,60), titleBlock=(40,44,52),
         axisLabel=(255,170,60), frame=(60,220,210), selBand=(50,56,66)),
    dict(outer=(8,10,18), panel=(14,18,30), ink=(230,240,255), digit=(255,220,80),
         underline=(0,255,220), title=(255,60,180), titleBlock=(24,28,44),
         axisLabel=(0,230,255), frame=(0,255,220), selBand=(20,26,42)),
    dict(outer=(246,242,232), panel=(238,232,218), ink=(50,48,44), digit=(40,70,150),
         underline=(90,140,120), title=(170,60,30), titleBlock=(222,216,202),
         axisLabel=(150,90,40), frame=(90,110,100), selBand=(232,224,206)),
]

CELLS_SRC0 = [(210,218,216), (217,227,224), (214,223,220), (39,39,39),
              (79,210,185), (25,54,170), (66,25,66)]
CELLS_TGT0 = lambda th: [th['outer'], th['outer'], th['outer'], th['ink'],
                         th['underline'], th['digit'], th['ink']]
CELLS_SRC1 = [(214,198,182), (222,200,179), (39,39,39), (79,210,185),
              (25,54,170), (66,25,66)]
CELLS_TGT1 = lambda th: [th['selBand'], th['selBand'], th['ink'],
                         th['underline'], th['digit'], th['ink']]
BG0 = (217,227,224)
CELLS_SRC2 = [lerp(c, BG0, 0.5) for c in CELLS_SRC0]
CELLS_TGT2 = lambda th: [lerp(t, th['outer'], 0.5) for t in CELLS_TGT0(th)]

for i, th in enumerate(THEMES):
    src = Image.open(os.path.join(A, "memoirs-cells.bmp")).convert("RGB")
    if i == 0:
        im = src.copy()          # ORIGINAL: keep every source pixel verbatim
    else:
        im = src.copy()
        px = im.load()
        w, h = im.size
        for y in range(h):
            row = y // 11
            srcs, tgts = ((CELLS_SRC0, CELLS_TGT0(th)) if row == 0 else
                          (CELLS_SRC1, CELLS_TGT1(th)) if row == 1 else
                          (CELLS_SRC2, CELLS_TGT2(th)))
            for x in range(w):
                px[x, y] = tgts[nearest(px[x, y], srcs)]
    px = im.load()
    w, h = im.size
    # row 3 = row 0 with the baked underline removed (info lines draw their
    # underline as one continuous vector strip AFTER both lines, otherwise
    # line 2's opaque cells overwrite line 1's underline).
    im4 = Image.new("RGB", (w, h + 11))
    im4.paste(im, (0, 0))
    p4 = im4.load()
    underline_rows = [y for y in range(11)
                      if any(px[x, y] == th['underline'] for x in range(w))]
    for y in range(11):
        for x in range(w):
            p4[x, h + y] = th['outer'] if y in underline_rows else px[x, y]
    # row 4 = frequency-axis label row: everything ink/digit/underline in the
    # theme axisLabel color on panel bg (chunky pixel weight, crisp).
    im5 = Image.new("RGB", (w, h + 22))
    im5.paste(im4, (0, 0))
    p5 = im5.load()
    bgset = {(210,218,216), (217,227,224), (214,223,220)} if i == 0 else {th['outer']}
    for y in range(11):
        for x in range(w):
            c = px[x, y]
            p5[x, h + 11 + y] = th['panel'] if c in bgset else th['axisLabel']
    im5.save(os.path.join(OUT, f"cells_{i}.bmp"))

    tsrc = Image.open(os.path.join(A, "memoirs-title.bmp")).convert("RGB")
    if i == 0:
        tsrc.save(os.path.join(OUT, f"title_{i}.bmp"))   # verbatim, no erosion
    else:
        px = tsrc.load(); w, h = tsrc.size
        for y in range(h):
            for x in range(w):
                c = px[x, y]
                px[x, y] = th['title'] if dist(c, (223,108,70)) < dist(c, (204,204,204)) \
                           else th['titleBlock']
        tsrc.save(os.path.join(OUT, f"title_{i}.bmp"))

    asrc = Image.open(os.path.join(A, "memoirs-axis.bmp")).convert("RGB")
    if i == 0:
        asrc.save(os.path.join(OUT, f"axis_{i}.bmp"))    # verbatim, no erosion
    else:
        px = asrc.load(); w, h = asrc.size
        for y in range(h):
            for x in range(w):
                c = px[x, y]
                if c[1] > 150 and c[2] > 150 and c[0] < 160:
                    px[x, y] = th['frame']
                elif c[0] > 150 and c[0] - c[2] > 60:
                    px[x, y] = th['axisLabel']
                else:
                    px[x, y] = th['panel']
        asrc.save(os.path.join(OUT, f"axis_{i}.bmp"))
    print("theme", i, "done")
print("wrote", OUT)