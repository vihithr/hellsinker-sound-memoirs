// Headless browser check: click the channel rail and assert the page switches.
// Uses Node's built-in WebSocket to talk to Chrome DevTools Protocol.
const tabs = await (await fetch("http://127.0.0.1:9223/json")).json();
const tab = tabs.find(t => t.type === "page" && t.url.includes("8100"));
if (!tab) throw new Error("no page tab");
const ws = new WebSocket(tab.webSocketDebuggerUrl);
await new Promise(r => ws.onopen = r);
let id = 0;
const pending = new Map();
ws.onmessage = e => {
  const m = JSON.parse(e.data);
  if (m.id && pending.has(m.id)) { pending.get(m.id)(m); pending.delete(m.id); }
};
function call(method, params = {}) {
  return new Promise(resolve => {
    const n = ++id; pending.set(n, resolve);
    ws.send(JSON.stringify({ id: n, method, params }));
  });
}
await call("Runtime.enable");
async function evalJs(expr) {
  const r = await call("Runtime.evaluate", { expression: expr, returnByValue: true, awaitPromise: true });
  return r.result?.result?.value;
}
// Wait for the app test hook.
for (let i = 0; i < 40; i++) {
  if (await evalJs("!!window.__hsTest")) break;
  await new Promise(r => setTimeout(r, 150));
}
console.log("hook ready:", await evalJs("!!window.__hsTest"));
console.log("initial page:", await evalJs("window.__hsTest?.page"));
// Channel rail: TR=366..406, LIB=406..446, SYS=446..486 (y 272..288).
console.log("click LIB x=426:", await evalJs("window.__hsTest.clickCanvas(426,280); document.body.dataset.page"));
console.log("click SYS x=466:", await evalJs("window.__hsTest.clickCanvas(466,280); document.body.dataset.page"));
console.log("click TR  x=386:", await evalJs("window.__hsTest.clickCanvas(386,280); document.body.dataset.page"));
ws.close();
