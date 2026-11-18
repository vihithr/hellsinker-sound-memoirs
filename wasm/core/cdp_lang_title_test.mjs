// End-to-end check on the local build:
//  1) language switch really repaints the SYS page (EN -> ZH)
//  2) the title bitmap really follows the theme (ORIGINAL -> DARK)
// Evidence: page screenshots before/after, compared off-band with PIL.
import { writeFileSync } from "node:fs";
const port = 9223;
const tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
if (!tab) throw new Error("no tab");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise(resolve => { const n = ++id; pending.set(n, resolve); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Page.enable");
const ev = async expr => (await call("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;
const shot = async name => {
  const s = await call("Page.captureScreenshot", { format: "png" });
  writeFileSync(`obj/${name}.png`, Buffer.from(s.result.data, "base64"));
};
// Fresh load, settled.
await call("Page.reload", { ignoreCache: true });
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
R.hook = await ev("!!window.__hsTest");
R.appTitle = await ev("document.title");
// --- 1) language switch repaints SYS text ---
await ev("window.__hsTest.clickCanvas(466,280)");           // SYS page
await new Promise(r=>setTimeout(r,400));
await shot("v_lang_en");
R.lang0 = await ev("window.__hsTest.lang");
// row 2 = LANGUAGE, ArrowRight switches to ZH
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight'}))");
await new Promise(r=>setTimeout(r,600));                     // font + frame
await shot("v_lang_zh");
R.lang1 = await ev("window.__hsTest.lang");
R.zhFont = await ev("document.fonts.check('12px FusionPixel')");
// --- 2) title follows theme ---
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'c'}))"); // DARK
await new Promise(r=>setTimeout(r,400));
await shot("v_title_dark");
R.theme = await ev("window.__hsTest.theme");
console.log(JSON.stringify(R, null, 2));
ws.close();
