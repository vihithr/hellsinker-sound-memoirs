// Verify prev/next buttons, double-click-to-play and scrubbing the seek rail.
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
R.buttons = await ev("['aux-prev','aux-play','aux-next'].filter(id=>!!document.getElementById(id))");
const doc = await call("DOM.getDocument");
const node = await call("DOM.querySelector", { nodeId: doc.result.root.nodeId, selector: "#files" });
await call("DOM.setFileInputFiles", { nodeId: node.result.nodeId, files: process.argv.slice(2) });
await new Promise(r=>setTimeout(r,2800));
R.tracks = await ev("window.__hsTest.visible.length");
await ev("window.__hsTest.setPage(1)");
await new Promise(r=>setTimeout(r,400));
// 1) play, then next
await ev("document.getElementById('aux-play').click()");
await new Promise(r=>setTimeout(r,1800));
R.playingAfterPlay = await ev("window.__hsTest.playing");
const t1 = await ev("window.__hsTest.currentRow");
R.curRow1 = t1;
await ev("document.getElementById('aux-next').click()");
await new Promise(r=>setTimeout(r,2000));
R.curRow2 = await ev("window.__hsTest.currentRow");
R.nextChanged = R.curRow1 !== R.curRow2;
R.playingAfterNext = await ev("window.__hsTest.playing");
// 2) prev goes back
await ev("document.getElementById('aux-prev').click()");
await new Promise(r=>setTimeout(r,2000));
R.curRow3 = await ev("window.__hsTest.currentRow");
R.prevChanged = R.curRow3 !== R.curRow2;
// 3) double-click row 1 (index 1) to play it
let dragRes = await ev(`(()=>{
  const s=document.getElementById('screen'), r=s.getBoundingClientRect();
  const P=(t,x,y)=>s.dispatchEvent(new PointerEvent(t,{clientX:r.left+x*(r.width/640),clientY:r.top+y*(r.height/480),bubbles:true,pointerId:1,isPrimary:true}));
  P('pointerdown',300,190); P('pointerup',300,190);
  return true;
})()`);
// 4) scrub to 80% of the rail
R.beforeDragMs = await ev("window.__hsTest.positionMs");
R.durMs = await ev("(()=>Math.round(window.__hsTest.bufferDuration*1000))()");
await ev(`(()=>{
  const s=document.getElementById('screen'), r=s.getBoundingClientRect();
  const P=(t,x,y)=>s.dispatchEvent(new PointerEvent(t,{clientX:r.left+x*(r.width/640),clientY:r.top+y*(r.height/480),bubbles:true,pointerId:2,isPrimary:true}));
  P('pointerdown',160,267); P('pointermove',420,267); P('pointerup',420,267);
  return true;
})()`);
await new Promise(r=>setTimeout(r,1200));
R.afterDragMs = await ev("window.__hsTest.positionMs");
R.dragRatio = R.durMs ? +(R.afterDragMs / R.durMs).toFixed(2) : 0;
R.playingAfterDrag = await ev("window.__hsTest.playing");
console.log(JSON.stringify(R, null, 2));
ws.close();
