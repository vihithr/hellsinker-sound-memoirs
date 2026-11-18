// Verify the four LIB order modes actually advance playback.
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
const sleep = ms => new Promise(r=>setTimeout(r,ms));
await call("Page.reload", { ignoreCache: true });
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await sleep(150); }
const R = {};
const doc = await call("DOM.getDocument");
const node = await call("DOM.querySelector", { nodeId: doc.result.root.nodeId, selector: "#files" });
await call("DOM.setFileInputFiles", { nodeId: node.result.nodeId, files: process.argv.slice(2) });
await sleep(2600);
await ev("window.__hsTest.setPage(1)");
await sleep(300);
R.tracks = await ev("window.__hsTest.visible.length");
// helper: play row N
const playRow = async n => {
  await ev(`(()=>{const t=window.__hsTest;[0,1,2,3,4,5,6,7].forEach(()=>{});})()`);
  await ev(`window.__hsTest.libPlay(${n})`);
};
// --- ORDER: row0 -> after end should be row1
await ev("window.__hsTest.localOrder=0");
await ev("window.__hsTest.stopAll()"); await sleep(300);
await ev("window.__hsTest.libPlay(0)");
await sleep(3400);
R.order_row = await ev("window.__hsTest.currentRow");
R.order_playing = await ev("window.__hsTest.playing");
// --- LOOP: start on last row, after end should wrap to row0
await ev("window.__hsTest.localOrder=2");
await ev("window.__hsTest.stopAll()"); await sleep(300);
await ev(`window.__hsTest.libPlay(${R.tracks - 1})`);
await sleep(3400);
R.loop_row = await ev("window.__hsTest.currentRow");
R.loop_playing = await ev("window.__hsTest.playing");
// --- ONE: should stay on the same row
await ev("window.__hsTest.localOrder=1");
await ev("window.__hsTest.stopAll()"); await sleep(300);
await ev("window.__hsTest.libPlay(0)");
await sleep(3400);
R.one_row = await ev("window.__hsTest.currentRow");
R.one_playing = await ev("window.__hsTest.playing");
// --- SHUFFLE: from row0 with 2 tracks it must land on row1
await ev("window.__hsTest.localOrder=3");
await ev("window.__hsTest.stopAll()"); await sleep(300);
await ev("window.__hsTest.libPlay(0)");
await sleep(3400);
R.shuffle_row = await ev("window.__hsTest.currentRow");
R.shuffle_playing = await ev("window.__hsTest.playing");
console.log(JSON.stringify(R, null, 2));
ws.close();
