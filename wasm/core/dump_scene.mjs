// Export one deterministic frame's command stream for pixel comparison with
// the desktop --shot baseline. State mirrors hsbgm.exe --shot: tick 200,
// silent spectrum, no current track, ORIGINAL theme.
import factory from "./core.js";
import { writeFileSync } from "node:fs";

const core = await factory();
const size = core._scene_state_size();
const ptr = core._alloc(size);
const base = core.HEAPU8.byteOffset + ptr;
const dv = new DataView(core.HEAPU8.buffer, base);
new Float32Array(core.HEAPU8.buffer, base, 611).fill(0);

dv.setInt32(2452, 200, true);   // tick (--shot settled frame)
dv.setInt32(2456, 0, true);     // selected
dv.setInt32(2460, -1, true);    // current: <SILENCE>
for (let i = 0; i < 46; i++) dv.setInt32(2464 + i * 4, 0, true);
dv.setInt32(2648, 0, true);     // theme ORIGINAL
dv.setInt32(2652, 0, true); dv.setInt32(2656, 0, true);
dv.setInt32(2660, 0, true); dv.setInt32(2664, 0, true);
dv.setInt32(2668, -1, true);
dv.setFloat64(2672, 0, true);
dv.setInt32(2680, 0, true);
// Extended fields from the shared scene_state.h: --shot baseline leaves the
// rail activity at tick 0, no local title, no page switch, no overlay/duration.
dv.setInt32(2684, 0, true);     // loading
dv.setInt32(2688, 0, true);     // railActivityTick
// localTitle (2692, 28 bytes) stays zeroed = empty.
dv.setInt32(2720, 0, true);     // switchTick
dv.setInt32(2724, 0, true);     // showGridOverlay
dv.setFloat64(2728, 0, true);   // duration

core._scene_begin();
core._scene_set_state(ptr);
core._scene_advance(-1, 0, 0, 0);
core._scene_render();
const n = core._scene_count();
const cmdPtr = core._scene_commands() >>> 0;
const ptPtr = core._scene_points() >>> 0;
const cmdBytes = new Uint8Array(core.HEAPU8.buffer,
  core.HEAPU8.byteOffset + cmdPtr, n * 20);

let maxPoint = 0;
const cmdView = new DataView(core.HEAPU8.buffer, core.HEAPU8.byteOffset + cmdPtr, n * 20);
for (let i = 0; i < n; i++) {
  const po = cmdView.getUint16(i * 20 + 16, true);
  const pc = cmdView.getUint16(i * 20 + 18, true);
  maxPoint = Math.max(maxPoint, po + pc);
}
const ptBytes = new Uint8Array(core.HEAPU8.buffer,
  core.HEAPU8.byteOffset + ptPtr, maxPoint * 4);

const out = new Uint8Array(4 + n * 20 + 4 + ptBytes.length);
const outView = new DataView(out.buffer);
outView.setUint32(0, n, true);
out.set(cmdBytes, 4);
outView.setUint32(4 + n * 20, maxPoint, true);
out.set(ptBytes, 8 + n * 20);
writeFileSync(process.argv[2] || "obj/scene_dump.bin", out);
console.log(`dumped ${n} commands, ${maxPoint} points`);
