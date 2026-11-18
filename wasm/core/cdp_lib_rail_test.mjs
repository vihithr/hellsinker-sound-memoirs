// Verify: hidden side rail, quick pause button, and a working LIB list.
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8111"));
if (!tab) throw new Error("no tab");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => { const m = JSON.parse(e.data); if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); } };
const call = (method, params = {}) => new Promise(res => { const n = ++id; pending.set(n, res); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Page.enable");
const ev = async e => (await call("Runtime.evaluate", { expression: e, returnByValue: true, awaitPromise: true })).result?.result?.value;
await call("Page.reload", { ignoreCache: true });
for (let i = 0; i < 40; i++) { if (await ev("!!window.__hsTest")) break; await new Promise(r=>setTimeout(r,150)); }
const R = {};
// 1) side rail hidden while idle (wakeAux runs at init, so wait it out)
await new Promise(r=>setTimeout(r,3000));
R.auxIdle = await ev("getComputedStyle(document.getElementById('aux')).display");
// and it appears on interaction
await ev("window.dispatchEvent(new KeyboardEvent('keydown',{key:'ArrowDown'}))");
R.auxAwake = await ev("getComputedStyle(document.getElementById('aux')).display");
// 2) pause button present and wired
R.pauseBtn = await ev("!!document.getElementById('aux-pause')");
R.pauseClickSafe = await ev("(()=>{try{document.getElementById('aux-pause').click();return 'ok'}catch(e){return 'ERR '+e.message}})()");
// 3) LIB list: inject sample local names through the real pipeline
await ev("window.__hsTest.clickCanvas(466,280)"); // SYS first (rail click)
R.pageSys = await ev("window.__hsTest.page");
await ev("window.__hsTest.setPage(1)");           // LIB
await new Promise(r=>setTimeout(r,300));
R.libEmptyHint = await ev("window.__hsTest.libNameCount===0 ? 'empty' : 'has'");
// feed names as the worker would report them
await ev(`(()=>{
  const files=['first_song.ogg','second_track.mp3','third_theme.wav'];
  window.__hsTest.setLocals(files);
  return true;
})()`);
await new Promise(r=>setTimeout(r,400));
R.visibleAfterFeed = await ev("window.__hsTest.visible");
R.namePoolCount = await ev("window.__hsTest.libNameCount");
// 4) search filter
await ev(`window.dispatchEvent(new KeyboardEvent('keydown',{key:'/'}))`);
await ev(`window.dispatchEvent(new KeyboardEvent('keydown',{key:'s'}))`);
await new Promise(r=>setTimeout(r,300));
R.visibleAfterSearchS = await ev("window.__hsTest.visible");
await ev(`window.dispatchEvent(new KeyboardEvent('keydown',{key:'Backspace'}))`);
await new Promise(r=>setTimeout(r,300));
R.visibleAfterBackspace = await ev("window.__hsTest.visible");
console.log(JSON.stringify(R, null, 2));
ws.close();
