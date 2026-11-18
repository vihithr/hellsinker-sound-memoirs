// Verify a CJK-named MP3 imports, renders its real name, and plays.
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
if (!tab) throw new Error("no tab");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise(res => { const n = ++id; pending.set(n, res); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Page.enable"); await call("DOM.enable");
const ev = async e => (await call("Runtime.evaluate", { expression: e, returnByValue: true, awaitPromise: true })).result?.result?.value;
await call("Page.reload", { ignoreCache: true });
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
R.hook = await ev("!!window.__hsTest");
if (!R.hook) { console.log(JSON.stringify(R)); ws.close(); process.exit(0); }
const doc = await call("DOM.getDocument");
const node = await call("DOM.querySelector", { nodeId: doc.result.root.nodeId, selector: "#files" });
await call("DOM.setFileInputFiles", { nodeId: node.result.nodeId, files: [process.argv[2]] });
await new Promise(r=>setTimeout(r,2500));
R.localFilesSize = await ev("window.__hsTest.localFilesSize");
R.visible = await ev("window.__hsTest.visible");
R.title = await ev("document.title");
// LIB page and let a frame render with the name pool.
await ev("window.__hsTest.setPage(1)");
await new Promise(r=>setTimeout(r,600));
const texts = await ev("window.__hsTest.drawnText()");
R.drawnTextRuns = texts ? texts.length : 0;
R.textsWithCJK = (texts || []).filter(s => /[\u4e00-\u9fff]/.test(s));
R.hasQuestionMarks = (texts || []).filter(s => s.includes("?"));
// play it
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'Enter'}))");
await new Promise(r=>setTimeout(r,2200));
R.playing = await ev("window.__hsTest.playing");
R.bufferDuration = await ev("window.__hsTest.bufferDuration");
R.positionMs = await ev("window.__hsTest.positionMs");
R.lastError = await ev("window.__hsTest.lastError");
console.log(JSON.stringify(R, null, 2));
ws.close();
