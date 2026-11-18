const port = 9224;
const tabs = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
const tab = tabs.find(t => t.type === "page");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => {
  const m = JSON.parse(e.data);
  if (m.method === "Runtime.exceptionThrown") console.log("EXC", m.params.exceptionDetails.exception?.description || m.params.exceptionDetails.text);
  if (m.method === "Log.entryAdded") console.log("LOG", m.params.entry.level, m.params.entry.text);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
const call = (method, params = {}) => new Promise(resolve => { const n = ++id; pending.set(n, resolve); ws.send(JSON.stringify({ id: n, method, params })); });
await call("Runtime.enable"); await call("Log.enable"); await call("Page.enable");
await call("Page.reload", { ignoreCache: true });
await new Promise(r => setTimeout(r, 6000));
const ev = async expr => (await call("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true })).result?.result?.value;
const R = {};
R.title = await ev("document.title");
R.body = await ev("document.body.innerText.slice(0,300)");
R.canvas = await ev("!!document.getElementById('screen')");
R.hook = await ev("!!window.__hsTest");
R.buttons = await ev("['aux-stage','aux-wing','aux-layout','aux-theme'].filter(id=>!!document.getElementById(id))");
R.url = await ev("location.href");
console.log(JSON.stringify(R, null, 2));
ws.close();
