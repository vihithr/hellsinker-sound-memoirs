// Diagnose the MP3 path: import a real mp3 and capture what happens.
import { writeFileSync } from "node:fs";
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
if (!tab) throw new Error("no tab");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
const logs = [];
ws.onmessage = e => {
  const m = JSON.parse(e.data);
  if (m.method === "Runtime.consoleAPICalled")
    logs.push('CONSOLE ' + m.params.args.map(a => a.value ?? a.description ?? '').join(' '));
  if (m.method === "Runtime.exceptionThrown")
    logs.push('EXCEPTION ' + (m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text));
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
const call = (method, params = {}) => new Promise(res => { const n = ++id; pending.set(n, res); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Page.enable"); await call("DOM.enable");
const ev = async e => (await call("Runtime.evaluate", { expression: e, returnByValue: true, awaitPromise: true })).result?.result?.value;
await call("Page.reload", { ignoreCache: true });
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
R.hook = await ev("!!window.__hsTest");
// Import the mp3 through the real input.
const doc = await call("DOM.getDocument");
const node = await call("DOM.querySelector", { nodeId: doc.result.root.nodeId, selector: "#files" });
await call("DOM.setFileInputFiles", { nodeId: node.result.nodeId, files: [process.argv[2]] });
await new Promise(r=>setTimeout(r,2000));
R.locals = await ev("window.__hsTest.visible");
R.localFileMapSize = await ev("window.__hsTest.localFilesSize");
R.statusText = await ev("document.title");
// Try to play it.
await ev("window.__hsTest.setPage(1)");
await new Promise(r=>setTimeout(r,300));
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter'}))");
await new Promise(r=>setTimeout(r,2500));
R.playing = await ev("window.__hsTest.playing");
R.bufferDuration = await ev("window.__hsTest.bufferDuration");
R.positionMs = await ev("window.__hsTest.positionMs");
R.lastError = await ev("window.__hsTest.lastError");
R.logs = logs.slice(-12);
console.log(JSON.stringify(R, null, 2));
ws.close();
