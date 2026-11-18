// Verify the mobile aux buttons switch stage / wing / layout / theme.
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
R.buttonsExist = await ev("['aux-stage','aux-wing','aux-layout','aux-theme'].every(id=>!!document.getElementById(id))");
R.stage0 = await ev("window.__hsTest.stage");
await ev("document.getElementById('aux-stage').click()"); R.stage1 = await ev("window.__hsTest.stage");
R.wing0 = await ev("window.__hsTest.wing");
await ev("document.getElementById('aux-wing').click()"); R.wing1 = await ev("window.__hsTest.wing");
R.layout0 = await ev("window.__hsTest.layout");
await ev("document.getElementById('aux-layout').click()"); R.layout1 = await ev("window.__hsTest.layout");
const th0 = await ev("window.__hsTest.theme");
await ev("document.getElementById('aux-theme').click()"); const th1 = await ev("window.__hsTest.theme");
R.themeChanged = th0 !== th1;
console.log(JSON.stringify(R, null, 2));
ws.close();
