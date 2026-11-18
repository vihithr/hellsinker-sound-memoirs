// Strict verification: read scene state fields directly from WASM memory.
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise(resolve => { const n = ++id; pending.set(n, resolve); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable");
await call("Page.enable");
await call("Page.reload", { ignoreCache: true });
await new Promise(r=>setTimeout(r,2500));
const ev = async expr => (await call("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;

for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
// Enter SYS page via rail click
await ev("window.__hsTest.clickCanvas(466,280)");
R.pageSYS = await ev("window.__hsTest.page");
// Move to language row (row 2) by two ArrowDowns
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))");
R.settingRow = await ev("window.__hsTest.settingRow");
// Switch language to ZH
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowRight'}))");
await new Promise(r=>setTimeout(r,300));
R.lang = await ev("window.__hsTest.lang");
R.fontZH = await ev("document.fonts.check('12px FusionPixel')");
// Switch theme via C
const t0 = await ev("document.title");
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'c'}))");
R.themeChanged = (await ev("document.title")) !== t0;
// Layout switch F9
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'F9'}))");
R.layout = await ev("window.__hsTest.layout");
console.log(JSON.stringify(R, null, 2));
ws.close();
