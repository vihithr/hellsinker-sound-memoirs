const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8100"));
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0; const pending = new Map();
ws.onmessage = e => {
  const m = JSON.parse(e.data);
  if (m.method === "Runtime.consoleAPICalled") console.log("CONSOLE", m.params.type, m.params.args.map(a=>a.value||a.description).join(" "));
  if (m.method === "Runtime.exceptionThrown") console.log("EXCEPTION", m.params.exceptionDetails.text, m.params.exceptionDetails.exception?.description);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
const call = (method, params = {}) => new Promise(resolve => {
  const n = ++id; pending.set(n, resolve); ws.send(JSON.stringify({ id: n, method, params }));
});
await call("Runtime.enable");
await call("Log.enable");
await call("Page.enable");
await call("Page.reload", { ignoreCache: true });
await new Promise(r => setTimeout(r, 4000));
const r = await call("Runtime.evaluate", { expression: "({title:document.title, body:document.body.innerText.slice(0,200), canvas:!!document.getElementById('screen'), hook:!!window.__hsTest})", returnByValue: true });
console.log("STATE", JSON.stringify(r.result?.result?.value));
ws.close();
