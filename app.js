import factory from "./core/core.js";
import {createScene} from "./scene.js";
const $=id=>document.getElementById(id);
const screen=$("screen"), files=$("files");

// ---- state (mirrors the desktop PlayerApp subset the replica needs) --------
let names=[],locals=[],available=[],selected=0,current=null;
let context=null,buffer=null,source=null,gain=null,vizCore=null,vizPtr=0,statePtr=0,stateView=null;
let offset=0,started=0,epoch=0,loading=false,theme=0,volume=80,stageMode=0,wingMode=0,layoutMode=0;
let tick=0,paused=false,railActivityTick=0,switchTick=0,showGridOverlay=false;
let playlist=[],localIndex=-1,localOrder=0; // 0 sequential, 1 repeat-one, 2 repeat-all, 3 shuffle
let scrubbing=false,scrubFraction=0,next=0,fatal=null,ready=false;
const THEMES=["ORIGINAL","DARK","NEON","PAPER","BLUEPRINT"];
const ORDERS=["ORDER","ONE","LOOP","SHUFFLE"];
const pending=new Map(),worker=new Worker("./worker.js",{type:"module"});

// ---- worker RPC -----------------------------------------------------------
function failWorker(message){fatal=message;for(const p of pending.values())p.reject(Error(message));pending.clear();}
worker.onerror=e=>failWorker(e.message||"无法启动 WASM 核心");
worker.onmessage=({data})=>{const p=pending.get(data.id);if(!p)return;pending.delete(data.id);data.error?p.reject(Error(data.error)):p.resolve(data);};
function request(op,args={}){return new Promise((resolve,reject)=>{
  if(fatal){reject(Error(fatal));return;}const id=++next;pending.set(id,{resolve,reject});worker.postMessage({id,op,...args});});}

// ---- title / chrome -------------------------------------------------------
function refreshTitle(){
  const t=THEMES[theme];
  document.title=`SOUND MEMOIRS  [VOL ${volume}] [${t}]`+(loading?"  [LOADING]":"");
}
function markRailActivity(){railActivityTick=tick;}
function switchPage(){switchTick=tick;}

// ---- playback -------------------------------------------------------------
function canPlay(i){return i<names.length?!!available[i]:i<names.length+locals.length;}
function position(){
  if(!buffer)return 0;
  const time=offset+(source&&!paused?context.currentTime-started:0);
  return source?.loop?time%buffer.duration:Math.min(buffer.duration,time);
}
function detach(){if(source){source.onended=null;try{source.stop();}catch(e){}source.disconnect();source=null;}}
function updateControls(){/* all UI is drawn in-canvas; nothing to enable/disable */}
function reset(){epoch++;detach();buffer=null;offset=0;current=null;loading=false;paused=false;refreshTitle();}
function select(i){if(i<0||i>=names.length+locals.length)return;selected=i;markRailActivity();}
function start(){
  detach();if(!buffer)return;
  if(offset>=buffer.duration)offset=0;
  const s=context.createBufferSource();s.buffer=buffer;s.loop=current!==null&&current<names.length;
  if(!gain){gain=context.createGain();gain.gain.value=volume/100;gain.connect(context.destination);}
  s.connect(gain);source=s;started=context.currentTime;paused=false;
  s.onended=()=>{if(source===s&&buffer&&s.loop!==true){source=null;offset=0;}};
  s.start(0,offset);
}
async function activate(){
  if(!canPlay(selected))return;
  if(source&&current===selected&&!paused){offset=position();detach();paused=true;refreshTitle();return;}
  if(source&&current===selected&&paused){paused=false;start();refreshTitle();return;}
  const id=++epoch,target=selected;
  try{
    if(!context)context=new AudioContext();
    await context.resume();if(id!==epoch)return;
    if(buffer&&current===target){loading=false;start();refreshTitle();return;}
    detach();buffer=null;current=null;offset=0;loading=true;refreshTitle();
    const {pcm}=await request("decode",{key:target<names.length?{name:names[target]}:{local:locals[target-names.length]}});
    if(id!==epoch)return;
    const b=context.createBuffer(2,pcm.length/2,44100);
    for(let c=0;c<2;c++){const out=b.getChannelData(c);for(let i=0;i<out.length;i++)out[i]=pcm[2*i+c]/32768;}
    buffer=b;current=target;loading=false;refreshTitle();start();
  }catch(e){if(id===epoch){reset();}}
}
function seek(seconds){if(!buffer)return;offset=Math.max(0,Math.min(buffer.duration,seconds));if(source)start();}
function setVolume(v){
  volume=Math.max(0,Math.min(100,v));
  if(gain)gain.gain.value=volume/100;
  refreshTitle();markRailActivity();
}
function stop(){reset();}
function stepLocal(delta){
  const count=playlist.length;if(!count)return;
  let idx=localIndex+delta;
  if(localOrder===2)idx=(idx+count)%count;
  else if(localOrder===3)idx=count<2?0:(idx+count)%count;
  if(idx<0||idx>=count)return;
  localIndex=idx;selected=names.length+idx;activate();
}
function cycleTheme(){theme=(theme+1)%THEMES.length;refreshTitle();markRailActivity();}
function cycleStage(){stageMode=(stageMode+1)%4;markRailActivity();}
function cycleWing(){wingMode=(wingMode+1)%5;markRailActivity();}
function cycleLayout(){layoutMode=(layoutMode+1)%5;showGridOverlay=false;markRailActivity();}

function moveSelection(dcol,drow){
  if(selected>=names.length)return;
  let col=Math.floor(selected/10),row=selected%10;
  col+=dcol;row+=drow;
  if(row<0){if(col>0){col--;row=9;}else{selected=-1;return;}}
  if(row>=10){if(col<4){col++;row=0;}else row=9;}
  col=Math.max(0,Math.min(4,col));
  selected=Math.min(names.length-1,col*10+row);
  markRailActivity();
}

// ---- mouse / keyboard / drop ---------------------------------------------
function canvasPoint(e){
  const r=screen.getBoundingClientRect();
  return {x:(e.clientX-r.left)*640/r.width,y:(e.clientY-r.top)*480/r.height};
}
function hitTrack(x,y){
  if(x<156||x>=436||y<288||y>=448)return null;
  const i=Math.floor((x-156)/56)*10+Math.floor((y-288)/16);
  return i<names.length?i:null;
}
function hitExit(x,y){return x>=156&&x<212&&y>=272&&y<288;}
screen.addEventListener("click",e=>{
  const {x,y}=canvasPoint(e);
  if(hitExit(x,y)){stop();return;}
  const i=hitTrack(x,y);if(i!==null){select(i);activate();}
});
screen.addEventListener("dblclick",e=>{
  const {x,y}=canvasPoint(e);
  if(hitExit(x,y)){stop();return;}
});
screen.addEventListener("pointerdown",()=>{screen.focus();markRailActivity();});
window.addEventListener("keydown",e=>{
  const k=e.key;
  if((e.ctrlKey||e.metaKey)&&(k==="o"||k==="O")){e.preventDefault();files.click();return;}
  if((e.ctrlKey||e.metaKey)&&(k==="q"||k==="Q")){e.preventDefault();stop();return;}
  const d={ArrowRight:[1,0],ArrowLeft:[-1,0],ArrowDown:[0,1],ArrowUp:[0,-1]}[k];
  if(d){e.preventDefault();moveSelection(d[0],d[1]);return;}
  if(k==="Enter"||k===" "){e.preventDefault();if(selected<0){stop();}else activate();return;}
  if(k==="s"||k==="S"){stop();return;}
  if(k==="+"||k==="="||k==="Add"){setVolume(volume+5);return;}
  if(k==="-"||k==="_"||k==="Subtract"){setVolume(volume-5);return;}
  if(k==="c"||k==="C"||k==="t"||k==="T"){cycleTheme();return;}
  if(k==="n"||k==="N"||k==="PageDown"){stepLocal(1);return;}
  if(k==="p"||k==="P"||k==="PageUp"){stepLocal(-1);return;}
  if(k==="F7"){e.preventDefault();cycleStage();return;}
  if(k==="F8"){e.preventDefault();cycleWing();return;}
  if(k==="F9"){e.preventDefault();cycleLayout();return;}
  if(k==="Tab"&&(layoutMode===1||layoutMode===4)){e.preventDefault();showGridOverlay=!showGridOverlay;return;}
  if(k==="Escape"){stop();return;}
});
async function importFiles(picked){
  if(!picked||!picked.length)return;
  loading=true;refreshTitle();
  try{
    const result=await request("import",{files:picked,names});
    available=result.available;locals=result.locals;playlist=result.locals.slice();localIndex=playlist.length?0:-1;
    if(!canPlay(selected)){const i=available.findIndex(Boolean);selected=i>=0?i:playlist.length?names.length:0;}
    markRailActivity();
  }catch(err){}
  loading=false;refreshTitle();
}
window.addEventListener("dragover",e=>{e.preventDefault();markRailActivity();});
window.addEventListener("drop",e=>{
  e.preventDefault();
  importFiles([...e.dataTransfer.files]);
});
files.addEventListener("change",()=>{
  const picked=[...files.files];files.value="";
  importFiles(picked);
});

// ---- render loop ----------------------------------------------------------
try{
  vizCore=await factory();vizPtr=vizCore._alloc(4096);
  const stateSize=vizCore._scene_state_size();
  statePtr=vizCore._alloc(stateSize);
  if(!vizPtr||!statePtr)throw Error("WASM 内存分配失败");
  stateView=new DataView(vizCore.HEAPU8.buffer,vizCore.HEAPU8.byteOffset+statePtr);
  const draw=await createScene(screen,vizCore);
  names=await (await fetch("./assets/tracks.json")).json();
  refreshTitle();
  let last=-Infinity;
  const drawFrame=(now=0)=>{
    if(now-last>=1000/60-.5){
      last=now;tick++;
      const seconds=position();
      const playing=!!source&&!paused;
      if(playing&&buffer){
        const n=buffer.length,first=Math.floor(Math.abs(Math.floor(seconds*44100)*4-22050)/4)%n;
        const l=buffer.getChannelData(0),r=buffer.getChannelData(1),dst=vizCore.HEAP16;
        for(let i=0,j=first;i<1024;i++,j=(j+1)%n){
          dst[(vizPtr>>>1)+2*i]=Math.round(l[j]*32768);
          dst[(vizPtr>>>1)+2*i+1]=Math.round(r[j]*32768);
        }
        const ptr=vizCore._sm_spectrum(vizPtr);
        new Float32Array(vizCore.HEAPU8.buffer,vizCore.HEAPU8.byteOffset+statePtr,611)
          .set(new Float32Array(vizCore.HEAPU8.buffer,ptr,611));
      }else vizCore._sm_spectrum_reset();

      // SceneState (shared painter layout, scene_state.h).
      stateView.setInt32(2452,tick,true);
      stateView.setInt32(2456,selected,true);
      stateView.setInt32(2460,current===null?-1:current,true);
      for(let i=0;i<46;i++)stateView.setInt32(2464+i*4,available[i]?1:0,true);
      stateView.setInt32(2648,theme,true);
      stateView.setInt32(2652,stageMode,true);
      stateView.setInt32(2656,wingMode,true);
      stateView.setInt32(2660,layoutMode,true);
      stateView.setInt32(2664,0,true); // page: Tracks
      stateView.setInt32(2668,-1,true); // hoverChannel
      stateView.setFloat64(2672,seconds,true);
      stateView.setInt32(2680,current!==null&&current>=names.length?1:0,true);
      stateView.setInt32(2684,loading?1:0,true);
      stateView.setInt32(2688,railActivityTick,true);
      stateView.setInt32(2720,switchTick,true);
      stateView.setInt32(2724,showGridOverlay?1:0,true);
      stateView.setFloat64(2728,buffer?buffer.duration:0,true);

      vizCore._scene_begin();
      vizCore._scene_set_state(statePtr);
      vizCore._scene_advance(current===null?-1:current,seconds,playing?1:0,paused?1:0);
      vizCore._scene_render();
      const hasAny=available.some(Boolean)||locals.length>0;
      const hint=loading?"DECODING ...":hasAny?"":"CTRL+O LOAD PAK OR DROP AUDIO FILE";
      draw({hintTheme:theme,hint});
    }
    requestAnimationFrame(drawFrame);
  };
  drawFrame();await request("init");ready=true;screen.focus();
}catch(e){
  document.title="SOUND MEMOIRS  [INIT FAILED]";
}
