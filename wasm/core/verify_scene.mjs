// Headless verification of the shared scene painter: drives the WASM exports
// directly and asserts that a plausible frame produces a well-formed command
// stream across themes, animation phases, layouts and stage compositions.
// No browser is required.
import factory from "./core.js";

const core = await factory();
const size = core._scene_state_size();
if (size !== 2984) throw new Error(`unexpected SceneState size: ${size}`);
const ptr = core._alloc(size);
if (!ptr) throw new Error("alloc failed");
const base = core.HEAPU8.byteOffset + ptr;
const dv = new DataView(core.HEAPU8.buffer, base);
const spec = new Float32Array(core.HEAPU8.buffer, base, 611);

function fill({ theme = 0, tick = 40, selected = 7, current = 3, seconds = 3.25,
                stage = 0, wing = 0, layout = 0, page = 0, loading = 0,
                duration = 180.0, railActivityTick = 0, switchTick = 0,
                showGridOverlay = 0 } = {}) {
  spec.fill(0);
  for (let i = 0; i < 43; i++) {
    spec[i] = 10 + i;               // bars
    spec[43 + i] = 8 + i;           // barsPrev
    spec[86 + i] = 12 + i;          // barsLevel
  }
  for (let i = 0; i < 240; i++) {
    spec[129 + i] = Math.sin(i / 9) * 9;
    spec[369 + i] = Math.cos(i / 9) * 9;
  }
  spec[609] = 0.45; spec[610] = 0.38;
  dv.setInt32(2452, tick, true);
  dv.setInt32(2456, selected, true);
  dv.setInt32(2460, current, true);
  for (let i = 0; i < 46; i++) dv.setInt32(2464 + i * 4, 1, true);
  dv.setInt32(2648, theme, true);
  dv.setInt32(2652, stage, true);
  dv.setInt32(2656, wing, true);
  dv.setInt32(2660, layout, true);
  dv.setInt32(2664, page, true);
  dv.setInt32(2668, -1, true);          // hoverChannel
  dv.setFloat64(2672, seconds, true);
  dv.setInt32(2680, current >= 46 ? 1 : 0, true);
  dv.setInt32(2684, loading, true);
  dv.setInt32(2688, railActivityTick, true);
  // localTitle at 2692: 28 bytes, leave zeroed (empty).
  dv.setInt32(2720, switchTick, true);
  dv.setInt32(2724, showGridOverlay, true);
  dv.setFloat64(2728, duration, true);
}

const KNOWN_OPS = new Set([1, 2, 3, 4, 5, 6, 7, 8, 9, 10]);

function render() {
  core._scene_begin();
  core._scene_set_state(ptr);
  core._scene_advance(3, 3.25, 1, 0);
  core._scene_render();
  const n = core._scene_count();
  if (n < 100) throw new Error(`too few instructions: ${n}`);
  const cmds = new DataView(core.HEAPU8.buffer,
    core.HEAPU8.byteOffset + core._scene_commands(), n * 20);
  let rects = 0, blits = 0, lines = 0, alpha = 0, clips = 0;
  let clipDepth = 0;
  for (let i = 0; i < n; i++) {
    const op = cmds.getUint8(i * 20);
    const color = cmds.getUint32(i * 20 + 4, true);
    if (color > 0xFFFFFF) throw new Error(`bad color at ${i}`);
    if (!KNOWN_OPS.has(op)) throw new Error(`unknown op ${op} at ${i}`);
    if (op === 2) rects++;
    else if (op === 8) blits++;
    else if (op === 3) alpha++;
    else if (op === 4 || op === 5) lines++;
    else if (op === 9) { clips++; clipDepth++; }
    else if (op === 10) { clips++; clipDepth--; if (clipDepth < 0) throw new Error(`clip underflow at ${i}`); }
  }
  if (clipDepth !== 0) throw new Error(`unbalanced clip stack: ${clipDepth}`);
  // Layouts other than Original draw fewer atlas glyphs; only require a
  // non-trivial, well-formed stream here (pixel accuracy is checked separately).
  if (rects < 10 || blits < 5) throw new Error(`unexpected mix rect=${rects} blit=${blits}`);
  return { n, rects, blits, lines, alpha, clips };
}

const results = [];
for (let theme = 0; theme < 5; theme++) {
  fill({ theme });
  results.push({ theme, ...render() });
}
for (const tick of [0, 4, 8, 16, 32, 64, 200]) {
  fill({ tick });
  results.push({ tick, ...render() });
}
for (let stage = 1; stage <= 3; stage++) {
  fill({ stage });
  results.push({ stage, ...render() });
}
for (let layout = 1; layout <= 4; layout++) {
  fill({ layout, showGridOverlay: layout === 1 || layout === 4 ? 1 : 0 });
  results.push({ layout, ...render() });
}
fill({ selected: -1 });
results.push({ exit: true, ...render() });
fill({ current: -1 });
results.push({ silence: true, ...render() });
fill({ loading: 1 });
results.push({ loading: true, ...render() });
fill({ page: 1 });
results.push({ page: 1, ...render() });
fill({ page: 2 });
results.push({ page: 2, ...render() });

for (const r of results) console.log(JSON.stringify(r));
console.log(`PASS: ${results.length} scene frames rendered`);
