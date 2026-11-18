// Comprehensive headless check of theme / language / page / font features.
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
if (!tab) throw new Error("no page tab; tabs=" + tabs.map(t=>t.url).join(","));
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0;
const pending = new Map();
ws.onmessage = e => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
const call = (method, params = {}) => new Promise(resolve => {
  const n = ++id; pending.set(n, resolve);
  ws.send(JSON.stringify({ id: n, method, params }));
});
await call("Runtime.enable");
const ev = async expr => (await call("Runtime.evaluate",
  { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;

for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const out = {};
out.hook = await ev("!!window.__hsTest");
out.initialPage = await ev("window.__hsTest?.page");
out.fontsReady = await ev("document.fonts.check('12px FusionPixel')");
// Theme switch via keyboard C (dispatched to window)
out.themeTitleBefore = await ev("document.title");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'c'}))");
out.themeTitleAfterC = await ev("document.title");
// Go to SYS page and switch language twice (settingRow=2)
await ev("window.__hsTest.clickCanvas(466,280)"); // SYS
out.pageAfterSysClick = await ev("window.__hsTest?.page");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))"); // row 2 = language
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight'}))"); // ZH
await new Promise(r=>setTimeout(r,500));
out.fontAfterLangSwitch = await ev("document.fonts.check('12px FusionPixel')");
out.titleAfterLang = await ev("document.title");
// Back to TR
await ev("window.__hsTest.clickCanvas(386,280)");
out.pageAfterTrClick = await ev("window.__hsTest?.page");
console.log(JSON.stringify(out, null, 2));
ws.close();
