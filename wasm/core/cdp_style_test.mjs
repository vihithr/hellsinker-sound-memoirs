// Verify F7/F8/F9 style switches and LIB FONT switching end to end.
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise(resolve => { const n = ++id; pending.set(n, resolve); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Page.enable");
await call("Page.reload", { ignoreCache: true });
await new Promise(r => setTimeout(r, 2000));
const ev = async expr => (await call("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
const key = k => ev(`window.dispatchEvent(new KeyboardEvent('keydown',{key:'${k}'}))`);
// Style keys on TR page
R.stage0 = await ev("window.__hsTest.stage");
await key("F7"); R.stage1 = await ev("window.__hsTest.stage");
R.wing0 = await ev("window.__hsTest.wing");
await key("F8"); R.wing1 = await ev("window.__hsTest.wing");
R.layout0 = await ev("window.__hsTest.layout");
await key("F9"); R.layout1 = await ev("window.__hsTest.layout");
// LIB FONT switch: go SYS, move to row 3, toggle
await ev("window.__hsTest.clickCanvas(466,280)"); // SYS
for (let i=0;i<3;i++) await key("ArrowDown"); // row 3
R.settingRow = await ev("window.__hsTest.settingRow");
R.libFont0 = await ev("window.__hsTest.libFont");
await key("ArrowRight");
R.libFont1 = await ev("window.__hsTest.libFont");
console.log(JSON.stringify(R, null, 2));
ws.close();
