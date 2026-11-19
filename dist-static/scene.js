// Canvas 2D replay of the POD drawing instructions produced by the derived
// C++ painter. GDI lines are aliased, so they are plotted pixel by pixel with
// Bresenham instead of using Canvas stroke() (which always antialiases).
const GFX_CLEAR = 1, GFX_RECT = 2, GFX_ALPHA_RECT = 3, GFX_LINE = 4,
      GFX_POLYLINE = 5, GFX_POLYGON = 6, GFX_ELLIPSE = 7, GFX_BLIT = 8,
      GFX_PUSH_CLIP = 9, GFX_POP_CLIP = 10, GFX_ALPHA_ELLIPSE = 11;
const CMD_SIZE = 20;

export async function createScene(canvas, core) {
  const ctx = canvas.getContext("2d", { alpha: false });
  ctx.imageSmoothingEnabled = false;

  // Five pre-rendered themed 8x11 cell atlases, produced by make_wasm_assets.py.
  const atlases = await Promise.all([0, 1, 2, 3, 4].map(async i => {
    const img = new Image();
    img.src = `assets/cells_${i}.png`;
    await img.decode();
    return img;
  }));

  // Themed title artwork addressed by src >= kGfxImageBase (100):
  //   100..104 = title_0..4.png (per-theme recolor by make_themed_title.py)
  const loadImage = async name => {
    const img = new Image(); img.src = `assets/${name}.png`; await img.decode(); return img;
  };
  const images = {};
  for (let i = 0; i < 5; i++) images[100 + i] = await loadImage(`title_${i}`);

  // COLORREF is 0x00BBGGRR; CSS wants rgb(R,G,B).
  const css = c => `rgb(${c & 255},${(c >> 8) & 255},${(c >> 16) & 255})`;

  // Cached radial-glare sprites (offscreen canvases) keyed by size+color.
  // Building a Canvas gradient per beacon per frame is heavy; drawImage of a
  // pre-rendered disc is cheap. globalAlpha applies the per-frame brightness.
  const glareCache = new Map();
  function glareSprite(w, h, cr, cg, cb) {
    const key = w + "x" + h + ":" + cr + "," + cg + "," + cb;
    let s = glareCache.get(key);
    if (s) { glareCache.delete(key); glareCache.set(key, s); return s; }
    if (glareCache.size >= 96) {
      // LRU evict: a full cache must never permanently disable the glare
      const oldest = glareCache.keys().next().value;
      glareCache.delete(oldest);
    }
    s = document.createElement("canvas");
    s.width = w; s.height = h;
    const sc = s.getContext("2d");
    const g = sc.createRadialGradient(w / 2, h / 2, 0, w / 2, h / 2, Math.max(w, h) / 2);
    g.addColorStop(0, `rgba(${cr},${cg},${cb},1)`);
    g.addColorStop(1, `rgba(${cr},${cg},${cb},0)`);
    sc.fillStyle = g;
    sc.fillRect(0, 0, w, h);
    glareCache.set(key, s);
    return s;
  }

  function line(x0, y0, x1, y1, color, width) {
    x0 |= 0; y0 |= 0; x1 |= 0; y1 |= 0;
    // GDI MoveToEx/LineTo excludes the destination endpoint on 1px h-lines.
    if (width === 1 && y0 === y1) x1 += x1 > x0 ? -1 : 1;
    ctx.fillStyle = color;
    const dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    const dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    // GDI centers an even-width pen one pixel up/left of the logical coord.
    // The plot below subtracts off, so a positive value shifts up/left.
    const off = width >> 1;
    for (;;) {
      ctx.fillRect(x0 - off, y0 - off, width, width);
      if (x0 === x1 && y0 === y1) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  // ASCII cell text overlay, using the same atlas and glyph order as the
  // shared C++ painter (src/scene.cpp DrawCells).
  const GLYPHS = "ABCDEFGHIJKLNMOPQRSTUVWXYZ 0123456789_-.:;&<>*+";
  function drawHintText(text, x, y, theme) {
    const atlas = atlases[theme] || atlases[0];
    for (const ch of text) {
      let i = GLYPHS.indexOf(ch);
      if (i < 0) i = GLYPHS.indexOf(" ");
      ctx.drawImage(atlas, i * 8, 2 * 11, 8, 11, x - 4, y - 5, 8, 11);
      x += 8;
    }
  }

  return function draw({ hintTheme = 0, hint = "" } = {}) {
    const n = core._scene_count();
    if (!n) return;
    const heap = core.HEAPU8;
    const cmdPtr = core._scene_commands() >>> 0;
    const ptPtr = core._scene_points() >>> 0;
    const dv = new DataView(heap.buffer, heap.byteOffset + cmdPtr, n * CMD_SIZE);
    const pdv = new DataView(heap.buffer, heap.byteOffset + ptPtr);
    const px = i => pdv.getInt16(i * 4, true);
    const py = i => pdv.getInt16(i * 4 + 2, true);

    for (let i = 0; i < n; i++) {
      const b = i * CMD_SIZE;
      const op = dv.getUint8(b);
      const src = dv.getUint8(b + 1);
      const width = dv.getUint8(b + 2);
      const alpha = dv.getUint8(b + 3);
      const color = css(dv.getUint32(b + 4, true));
      const x0 = dv.getInt16(b + 8, true), y0 = dv.getInt16(b + 10, true);
      const x1 = dv.getInt16(b + 12, true), y1 = dv.getInt16(b + 14, true);
      const po = dv.getUint16(b + 16, true), pc = dv.getUint16(b + 18, true);
      switch (op) {
        case GFX_CLEAR:
          ctx.fillStyle = color; ctx.fillRect(0, 0, 640, 480); break;
        case GFX_RECT:
          ctx.fillStyle = color; ctx.fillRect(x0, y0, x1 - x0, y1 - y0); break;
        case GFX_ALPHA_RECT:
          ctx.globalAlpha = alpha / 255; ctx.fillStyle = color;
          ctx.fillRect(x0, y0, x1 - x0, y1 - y0); ctx.globalAlpha = 1; break;
        case GFX_LINE:
          line(x0, y0, x1, y1, color, width); break;
        case GFX_POLYLINE: {
          let ax = px(po), ay = py(po);
          for (let k = 1; k < pc; k++) {
            const bx = px(po + k), by = py(po + k);
            line(ax, ay, bx, by, color, width);
            ax = bx; ay = by;
          }
          break;
        }
        case GFX_POLYGON:
          ctx.fillStyle = color; ctx.beginPath();
          for (let k = 0; k < pc; k++) {
            if (k === 0) ctx.moveTo(px(po), py(po));
            else ctx.lineTo(px(po + k), py(po + k));
          }
          ctx.closePath(); ctx.fill(); break;
        case GFX_ELLIPSE:
          ctx.fillStyle = color; ctx.beginPath();
          ctx.ellipse((x0 + x1) / 2, (y0 + y1) / 2,
                      Math.abs(x1 - x0) / 2, Math.abs(y1 - y0) / 2,
                      0, 0, Math.PI * 2);
          ctx.fill(); break;
        case GFX_ALPHA_ELLIPSE: {
          // soft radial glare via a cached sprite; globalAlpha = brightness
          const w = Math.abs(x1 - x0), h = Math.abs(y1 - y0);
          if (w <= 0 || h <= 0 || w > 128 || h > 128) break;
          const m = /rgb\((\d+),(\d+),(\d+)\)/.exec(color);
          const cr = m ? +m[1] : 255, cg = m ? +m[2] : 255, cb = m ? +m[3] : 255;
          const s = glareSprite(w, h, cr, cg, cb);
          if (!s) break;
          ctx.save();
          ctx.globalAlpha = alpha / 255;
          ctx.drawImage(s, Math.min(x0, x1), Math.min(y0, y1), w, h);
          ctx.restore();
          break;
        }
        case GFX_BLIT: {
          // Shared gfx_blit carries destination size in the second point.
          // A {0,0} size falls back to the source cell size (revision-1 streams).
          const dw = px(po + 1) || x1, dh = py(po + 1) || y1;
          const source = src >= 100 ? (images[src] || images[100]) : (atlases[src] || atlases[0]);
          ctx.drawImage(source, x0, y0, x1, y1, px(po), py(po), dw, dh);
          break;
        }
        case GFX_PUSH_CLIP:
          ctx.save();
          ctx.beginPath();
          ctx.rect(x0, y0, x1 - x0, y1 - y0);
          ctx.clip();
          break;
        case GFX_POP_CLIP:
          ctx.restore();
          break;
        default:
          throw new Error(`unknown GfxOp ${op} at command ${i}`);
      }
    }

    // Unicode text commands (SYS/LIB pages), replayed with the CJK web font.
    const tc = core._scene_text_count();
    if (tc) {
      const textsPtr = core._scene_texts() >>> 0;
      const tpsPtr = core._scene_text_points() >>> 0;
      const tdv = new DataView(heap.buffer, heap.byteOffset + textsPtr, tc * 20);
      const tpdv = new DataView(heap.buffer, heap.byteOffset + tpsPtr);
      ctx.font = '12px "FusionPixel", monospace';
      ctx.textBaseline = "top";
      for (let i = 0; i < tc; i++) {
        const b = i * 20;
        const cpo = tdv.getUint16(b, true), cnt = tdv.getUint16(b + 2, true);
        const x = tdv.getInt16(b + 4, true), y = tdv.getInt16(b + 6, true);
        const right = tdv.getInt16(b + 8, true), bottom = tdv.getInt16(b + 10, true);
        const align = tdv.getUint8(b + 12);
        const color = tdv.getUint32(b + 16, true);
        let s = "";
        for (let k = 0; k < cnt; k++) s += String.fromCodePoint(tpdv.getUint32((cpo + k) * 4, true));
        if (!s) continue;
        ctx.fillStyle = `rgb(${color & 255},${(color >> 8) & 255},${(color >> 16) & 255})`;
        const w = ctx.measureText(s).width;
        let tx = x;
        if (align === 1) tx = x + (right - x - w) / 2;
        else if (align === 2) tx = right - w;
        ctx.fillText(s, tx, y + (bottom - y - 12) / 2);
      }
    }

    // Shell-owned hint line (not part of the shared pixel-exact scene).
    if (hint) drawHintText(hint, 144, 472, hintTheme);
  };
}
