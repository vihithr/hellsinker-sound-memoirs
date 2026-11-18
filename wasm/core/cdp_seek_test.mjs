// Verify the seek rail with real CDP mouse input (synthetic PointerEvents
// do not exercise pointer capture the way a device does).
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
const R = {};
R.hook = await ev("!!window.__hsTest");
const doc = await call("DOM.getDocument");
const node = await call("DOM.querySelector", { nodeId: doc.result.root.nodeId, selector: "#files" });
await call("DOM.setFileInputFiles", { nodeId: node.result.nodeId, files: process.argv.slice(2) });
await new Promise(r=>setTimeout(r,2800));
await ev("window.__hsTest.setPage(1)");
await new Promise(r=>setTimeout(r,400));
R.tracks = await ev("window.__hsTest.visible.length");
R.selected = await ev("window.__hsTest.selected");
await ev("document.getElementById('aux-play').click()");
await new Promise(r=>setTimeout(r,3000));
R.playing = await ev("window.__hsTest.playing");
R.lastError = await ev("window.__hsTest.lastError");
R.durMs = await ev("Math.round(window.__hsTest.bufferDuration*1000)");
R.beforeMs = await ev("window.__hsTest.positionMs");
const rect = await ev("(()=>{const b=document.getElementById('screen').getBoundingClientRect();return{left:b.left,top:b.top,width:b.width,height:b.height}})()");
const px = x => rect.left + x * (rect.width / 640);
const py = y => rect.top + y * (rect.height / 480);
// Drag the rail from 20% to ~50% (leaving headroom so it does not end).
await call("Input.dispatchMouseEvent", { type: "mousePressed", x: px(200), y: py(267), button: "left", clickCount: 1 });
await new Promise(r=>setTimeout(r,120));
for (const x of [240, 270, 300, 320]) {
  await call("Input.dispatchMouseEvent", { type: "mouseMoved", x: px(x), y: py(267), button: "left" });
  await new Promise(r=>setTimeout(r,60));
}
await call("Input.dispatchMouseEvent", { type: "mouseReleased", x: px(320), y: py(267), button: "left", clickCount: 1 });
await new Promise(r=>setTimeout(r,400));
R.afterMs = await ev("window.__hsTest.positionMs");
R.dragRatio = R.durMs ? +(R.afterMs / R.durMs).toFixed(2) : 0;
R.playingAfterDrag = await ev("window.__hsTest.playing");
// Double-click the second LIB row: it must start that track.
R.rowBeforeDbl = await ev("window.__hsTest.currentRow");
await call("Input.dispatchMouseEvent", { type: "mousePressed", x: px(300), y: py(322), button: "left", clickCount: 1 });
await call("Input.dispatchMouseEvent", { type: "mouseReleased", x: px(300), y: py(322), button: "left", clickCount: 1 });
await call("Input.dispatchMouseEvent", { type: "mousePressed", x: px(300), y: py(322), button: "left", clickCount: 2 });
await call("Input.dispatchMouseEvent", { type: "mouseReleased", x: px(300), y: py(322), button: "left", clickCount: 2 });
await new Promise(r=>setTimeout(r,2500));
R.rowAfterDbl = await ev("window.__hsTest.currentRow");
R.playingAfterDbl = await ev("window.__hsTest.playing");
R.lastError = await ev("window.__hsTest.lastError");
console.log(JSON.stringify(R, null, 2));
ws.close();
