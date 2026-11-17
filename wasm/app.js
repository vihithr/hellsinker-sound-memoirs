import factory from "./core/core.mjs";
import {renderer} from "./screen.js";
const $=id=>document.getElementById(id);
const screen=$("screen"), tracks=$("tracks"), play=$("play"), stop=$("stop"), seek=$("seek"), status=$("status");
let names=[],locals=[],available=[],selected=0,current=null,context,buffer,source,vizCore,vizPtr,offset=0,started=0,epoch=0,loading=false,scrubbing=false;
let next=0,fatal=null;
const pending=new Map(),worker=new Worker("./worker.js",{type:"module"});
function failWorker(message){fatal=message;for(const p of pending.values())p.reject(Error(message));pending.clear();status.textContent=message;}
worker.onerror=e=>failWorker(e.message || "无法启动 WASM 核心");
worker.onmessage=({data})=>{const p=pending.get(data.id);if(!p)return;pending.delete(data.id);data.error?p.reject(Error(data.error)):p.resolve(data);};
function request(op,args={}){return new Promise((resolve,reject)=>{
  if(fatal){reject(Error(fatal));return;}const id=++next;pending.set(id,{resolve,reject});worker.postMessage({id,op,...args});});}
const canPlay=i=>i<names.length?!!available[i]:i<names.length+locals.length;
function position(){
  if(!buffer)return 0;
  const time=offset+(source?context.currentTime-started:0);
  return source?.loop?time%buffer.duration:Math.min(buffer.duration,time);
}
function detach(){if(source){source.onended=null;source.stop();source.disconnect();source=null;}}
function controls(){play.disabled=!canPlay(selected);play.textContent=loading?"读取中":source&&current===selected?"暂停":"播放";stop.disabled=!buffer&&!loading;seek.disabled=!buffer;}
function reset(){epoch++;detach();buffer=null;offset=0;current=null;loading=false;scrubbing=false;seek.value=0;controls();}
function select(i){selected=i;tracks.value=String(i);controls();}
function options(){
  tracks.replaceChildren();
  for(const [i,name] of [...names,...locals].entries()){
    const o=document.createElement("option");o.value=i;o.textContent=(i<names.length?"TR:"+String(i).padStart(2,"0")+"  ":"LIB  ")+name+(canPlay(i)?"":" · 未载入");tracks.add(o);
  }
  select(selected);
}
function start(){
  detach();if(!buffer)return;
  if(offset>=buffer.duration)offset=0;
  const s=context.createBufferSource();s.buffer=buffer;s.loop=current!==null&&current<names.length;s.connect(context.destination);source=s;started=context.currentTime;
  s.onended=()=>{if(source===s){source=null;s.disconnect();offset=0;controls();}};
  s.start(0,offset);controls();
}
async function activate(){
  if(!canPlay(selected))return;
  if(source && current===selected){offset=position();detach();controls();return;}
  const id=++epoch, target=selected;
  try{
    if(!context){context=new AudioContext();}
    await context.resume();if(id!==epoch)return;
    if(buffer && current===target){loading=false;start();return;}
    detach();buffer=null;current=null;offset=0;loading=true;controls();status.textContent="正在解码…";
    const {pcm}=await request("decode",{key:target<names.length?{name:names[target]}:{local:locals[target-names.length]}});
    if(id!==epoch)return;
    const b=context.createBuffer(2,pcm.length/2,44100);
    for(let c=0;c<2;c++){const out=b.getChannelData(c);for(let i=0;i<out.length;i++)out[i]=pcm[2*i+c]/32768;}
    buffer=b;current=target;loading=false;status.textContent=target<names.length?names[target]:locals[target-names.length];start();
  }catch(e){if(id===epoch){reset();status.textContent=e.message;}}
}
tracks.onchange=()=>select(Number(tracks.value));play.onclick=activate;stop.onclick=reset;
seek.oninput=()=>{scrubbing=true;};
seek.onchange=()=>{if(buffer){const running=!!source;offset=Number(seek.value)/1000*buffer.duration;if(running)start();}scrubbing=false;};
seek.onpointercancel=()=>{scrubbing=false;};seek.onblur=()=>{scrubbing=false;};
screen.onclick=e=>{
  const r=screen.getBoundingClientRect(),x=(e.clientX-r.left)*640/r.width,y=(e.clientY-r.top)*480/r.height;
  if(x>=156&&x<212&&y>=272&&y<288){reset();return;}
  if(x<156||x>=436||y<288||y>=448)return;
  const i=Math.floor((x-156)/56)*10+Math.floor((y-288)/16);if(i<names.length){select(i);activate();}
};
screen.onkeydown=e=>{
  const d={ArrowRight:10,ArrowLeft:-10,ArrowDown:1,ArrowUp:-1}[e.key];
  if(d){e.preventDefault();select(Math.max(0,Math.min(names.length-1,selected+d)));}
  if(e.key==="Enter"||e.key===" "){e.preventDefault();activate();}
};
const files=$("files");files.disabled=true;
files.onchange=async()=>{
  const picked=[...files.files];files.value="";if(!picked.length)return;
  files.disabled=true;reset();
  try{
    const result=await request("import",{files:picked,names});available=result.available;locals=result.locals;
    if(!canPlay(selected)){const i=available.findIndex(Boolean);selected=i>=0?i:locals.length?names.length:0;}
    options();status.textContent="已载入 "+available.filter(Boolean).length+"/46 首 TR，"+locals.length+" 个本地文件。 "+result.errors.join("；");
  }catch(e){status.textContent=e.message;}finally{files.disabled=false;}
};
try{
  const draw=await renderer(screen);
  vizCore=await factory();vizPtr=vizCore._alloc(4096);
  if(!vizPtr)throw Error("频谱缓冲区分配失败");
  names=await (await fetch("./assets/tracks.json")).json();options();
  let last=-Infinity;
  const drawFrame=(now=0)=>{
    // The original envelope is evaluated at 60 Hz, independent of display refresh.
    if(now-last>=1000/60-.5){
      last=now;
      const seconds=position();
      let visual=null;
      if(source&&buffer){
        const n=buffer.length, first=Math.floor(Math.abs(Math.floor(seconds*44100)*4-22050)/4)%n;
        const l=buffer.getChannelData(0),r=buffer.getChannelData(1),dst=vizCore.HEAP16;
        for(let i=0,j=first;i<1024;i++,j=(j+1)%n){
          dst[(vizPtr>>>1)+2*i]=Math.round(l[j]*32768);dst[(vizPtr>>>1)+2*i+1]=Math.round(r[j]*32768);
        }
        const ptr=vizCore._sm_spectrum(vizPtr);
        visual=new Float32Array(vizCore.HEAPU8.buffer,ptr,611);
      } else vizCore._sm_spectrum_reset();
      draw({selected,current,available,seconds,visual});
      if(!scrubbing&&buffer)seek.value=String(Math.round(seconds/buffer.duration*1000));
    }
    requestAnimationFrame(drawFrame);
  };
  drawFrame();await request("init");files.disabled=false;status.textContent="选择 PAK 或音频文件以开始。";
}catch(e){status.textContent=e.message;}
