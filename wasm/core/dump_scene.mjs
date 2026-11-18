// Export one deterministic frame's command stream for pixel comparison with
// the desktop --shot baseline. State mirrors hsbgm.exe --shot: tick 200,
// silent spectrum, no current track, ORIGINAL theme.
import factory from "./core.js";
import { writeFileSync } from "node:fs";

// Optional CLI: [page] [settingRow] [volume] [language] [outputPath]
const page = Number(process.argv[2] ?? 0);
const settingRow = Number(process.argv[3] ?? 0);
const volume = Number(process.argv[4] ?? 80);
const language = Number(process.argv[5] ?? 0);
const output = process.argv[6] || "obj/scene_dump.bin";

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
dv.setInt32(2660, 0, true); dv.setInt32(2664, page, true);
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
// SYS / LIB shell fields (new POD tail).
dv.setInt32(2736, language, true);      // language
dv.setInt32(2740, volume, true);        // volume
dv.setInt32(2744, 0, true);             // libFont (matches --shot ini LibFont=0)
dv.setInt32(2748, settingRow, true);    // settingRow
// Desktop --shot leaves the composition name empty in this state.
dv.setInt32(2840, 0, true);             // layoutNameLen

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

// Text pool: textCount(u32) + texts(n*20) + textPointsCount(u32) + points(n*4).
const textCount = core._scene_text_count();
const textPtr = core._scene_text_points() >>> 0;
const textsPtr = core._scene_texts() >>> 0;
const textsBytes = new Uint8Array(core.HEAPU8.buffer,
  core.HEAPU8.byteOffset + textsPtr, textCount * 20);
let maxTextPoint = 0;
const textsView = new DataView(core.HEAPU8.buffer, core.HEAPU8.byteOffset + textsPtr, textCount * 20);
for (let i = 0; i < textCount; i++) {
  const off = textsView.getUint16(i * 20, true);
  const cnt = textsView.getUint16(i * 20 + 2, true);
  maxTextPoint = Math.max(maxTextPoint, off + cnt);
}
const textPointsBytes = new Uint8Array(core.HEAPU8.buffer,
  core.HEAPU8.byteOffset + textPtr, maxTextPoint * 4);

const head = 8 + n * 20 + ptBytes.length;
const out = new Uint8Array(head + 4 + textsBytes.length + 4 + textPointsBytes.length);
const outView = new DataView(out.buffer);
outView.setUint32(0, n, true);
out.set(cmdBytes, 4);
outView.setUint32(4 + n * 20, maxPoint, true);
out.set(ptBytes, 8 + n * 20);
outView.setUint32(head, textCount, true);
out.set(textsBytes, head + 4);
outView.setUint32(head + 4 + textsBytes.length, maxTextPoint, true);
out.set(textPointsBytes, head + 8 + textsBytes.length);
writeFileSync(output, out);
console.log(`dumped ${n} commands, ${maxPoint} points, ${textCount} texts, ${maxTextPoint} text points`);
