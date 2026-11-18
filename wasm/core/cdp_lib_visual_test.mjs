// Prove LIB renders real file names, and the pause button really pauses.
import { writeFileSync } from "node:fs";
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise(res => { const n = ++id; pending.set(n, res); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Page.enable"); await call("DOM.enable");
const ev = async e => (await call("Runtime.evaluate", { expression: e, returnByValue: true, awaitPromise: true })).result?.result?.value;
const shot = async name => {
  const s = await call("Page.captureScreenshot", { format: "png" });
  writeFileSync(`obj/${name}.png`, Buffer.from(s.result.data, "base64"));
};
await call("Page.reload", { ignoreCache: true });
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
// LIB page, empty state vs populated state
await ev("window.__hsTest.setPage(1)");
await new Promise(r=>setTimeout(r,400));
await shot("lib_empty");
// Load a real audio file through the actual <input type=file> path.
const doc = await call("DOM.getDocument");
const node = await call("DOM.querySelector", { nodeId: doc.result.root.nodeId, selector: "#files" });
await call("DOM.setFileInputFiles", { nodeId: node.result.nodeId, files: [process.argv[2]] });
await new Promise(r=>setTimeout(r,2500));
R.localsCount = await ev("window.__hsTest.visible.length");
await shot("lib_filled");
// Play it (Enter) then pause via the aux button.
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter'}))");
await new Promise(r=>setTimeout(r,1200));
R.playingAfterEnter = await ev("window.__hsTest.playing");
const before = await ev("window.__hsTest.positionMs");
await ev("document.getElementById('aux-pause').click()");
await new Promise(r=>setTimeout(r,600));
R.pausedAfterBtn = await ev("window.__hsTest.paused");
const p1 = await ev("window.__hsTest.positionMs");
await new Promise(r=>setTimeout(r,700));
const p2 = await ev("window.__hsTest.positionMs");
R.frozenWhilePaused = (p1 === p2);
await ev("document.getElementById('aux-pause').click()");
await new Promise(r=>setTimeout(r,700));
const p3 = await ev("window.__hsTest.positionMs");
R.resumed = p3 > p2;
await shot("lib_playing");
console.log(JSON.stringify(R, null, 2));
ws.close();
