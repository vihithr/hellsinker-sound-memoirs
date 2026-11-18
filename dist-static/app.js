import factory from "./core/core.js";
import {createScene} from "./scene.js";
const $=id=>document.getElementById(id);
const screen=$("screen"), files=$("files"), aux=$("aux");
let auxTimer=0;
function wakeAux(){
  aux.classList.add("visible");
  clearTimeout(auxTimer);
  auxTimer=setTimeout(()=>aux.classList.remove("visible"),2500);
}
function toggleFullscreen(){
  try{
    if(document.fullscreenElement){document.exitFullscreen();}
    else{document.documentElement.requestFullscreen?.();}
  }catch(e){}
  wakeAux();
}
$("aux-load")?.addEventListener("click",()=>{files.click();wakeAux();});
$("aux-full")?.addEventListener("click",toggleFullscreen);
$("aux-page")?.addEventListener("click",()=>{selectPage((page+1)%3);wakeAux();});

// ---- state (mirrors the desktop PlayerApp subset the replica needs) --------
let names=[],locals=[],available=[],selected=0,current=null;
let context=null,buffer=null,source=null,gain=null,vizCore=null,vizPtr=0,statePtr=0,stateView=null;
let offset=0,started=0,epoch=0,loading=false,theme=0,volume=80,stageMode=0,wingMode=0,layoutMode=0;
let tick=0,paused=false,railActivityTick=0,switchTick=0,showGridOverlay=false;
let page=0; // 0=TR, 1=LIB, 2=SYS
let settingRow=0,libFont=1;
let libCursor=0,libScroll=0,searchText="",searchActive=false;
function clampLibScroll(){if(libCursor<libScroll)libScroll=libCursor;if(libCursor>=libScroll+7)libScroll=libCursor-6;libScroll=Math.max(0,libScroll);}
let lang=0; // 0=EN, 1=ZH, 2=JA
let playlist=[],localIndex=-1,localOrder=0; // 0 sequential, 1 repeat-one, 2 repeat-all, 3 shuffle
let scrubbing=false,scrubFraction=0,next=0,fatal=null,ready=false;
const THEMES=["ORIGINAL","DARK","NEON","PAPER","BLUEPRINT"];
const LAYOUTS=["ORIGINAL","FIELD","STACK","RING","CROSS"];
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
function markRailActivity(){railActivityTick=tick;wakeAux();}
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
function selectPage(p){page=p;switchTick=tick;document.body.dataset.page=String(p);markRailActivity();}
function adjustSetting(direction){
  switch(settingRow){
    case 0: setVolume(volume+direction*5); break;
    case 1: theme=(theme+(direction>0?1:4))%5; refreshTitle(); break;
    case 2: lang=(lang+(direction>0?1:2))%3; loadFontForLang(); refreshTitle(); break;
    case 3: libFont=libFont?0:1; break;
    case 4: cycleLayout(); break;
  }
  markRailActivity();
}

// Load the matching CJK web font when the language needs one (ZH/JA).
async function loadFontForLang(){
  const name=lang===2?"fusion-ja":"fusion-zh";
  try{
    const font=new FontFace("FusionPixel",`url(assets/fonts/${name}.woff2)`);
    await font.load();document.fonts.add(font);
  }catch(e){}
}

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
  // Channel rail (TR/LIB/SYS) sits at x 362..486, y 272..288 on every page.
  if(y>=272&&y<288&&x>=366&&x<486){
    const ch=Math.floor((x-366)/40);
    if(ch>=0&&ch<3){selectPage(ch);return;}
  }
  if(page===1){ // LIB
    if(y>=293&&y<309){files.click();return;}
    if(y>=313&&y<439){const row=Math.floor((y-313)/18);const idx=libScroll+row;if(idx>=0&&idx<playlist.length){libCursor=idx;markRailActivity();}return;}
    if(y>=443&&y<460){
      if(x>=154&&x<212){localOrder=(localOrder+1)%4;markRailActivity();return;}
      if(x>=213&&x<277){if(buffer&&current!==null&&current>=names.length){offset=position();detach();paused=!paused;if(!paused)start();}return;}
      if(x>=278&&x<331){stop();return;}
    }
    return;
  }
  if(page===2){ // SYS
    if(y>=283&&y<438){const row=Math.floor((y-283)/31);if(row>=0&&row<5){settingRow=row;if(x>=269&&x<296)adjustSetting(-1);else if(x>=462&&x<486)adjustSetting(1);markRailActivity();}return;}
    return;
  }
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
  if(e.altKey&&k==="Enter"){e.preventDefault();toggleFullscreen();return;}
  if(e.ctrlKey||e.metaKey){
    if(k==="o"||k==="O"){e.preventDefault();files.click();return;}
    if(k==="q"||k==="Q"){e.preventDefault();stop();return;}
    if(k==="l"||k==="L"){e.preventDefault();selectPage(page===1?0:1);return;}
  }
  if(k==="F1"||k==="F2"||k==="F3"){e.preventDefault();selectPage(k==="F1"?0:k==="F2"?1:2);return;}
  if(page===1){ // LIB navigation
    if(k==="ArrowUp"){e.preventDefault();libCursor=Math.max(0,libCursor-1);clampLibScroll();markRailActivity();return;}
    if(k==="ArrowDown"){e.preventDefault();libCursor=Math.min(playlist.length-1,libCursor+1);clampLibScroll();markRailActivity();return;}
    if(k==="PageUp"){e.preventDefault();libCursor=Math.max(0,libCursor-7);clampLibScroll();return;}
    if(k==="PageDown"){e.preventDefault();libCursor=Math.min(playlist.length-1,libCursor+7);clampLibScroll();return;}
    if(k==="Enter"){e.preventDefault();if(playlist.length){localIndex=libCursor;selected=names.length+libCursor;activate();}return;}
    if(k===" "){e.preventDefault();if(buffer&&current!==null&&current>=names.length){offset=position();detach();paused=!paused;if(!paused)start();}return;}
    if(k==="ArrowLeft"||k==="ArrowRight"){e.preventDefault();seek(position()+(k==="ArrowLeft"?-5:5));return;}
    if(k==="/"){e.preventDefault();searchText="";searchActive=true;return;}
    if(searchActive&&k.length===1){searchText+=k;markRailActivity();return;}
    if(k==="Backspace"&&searchActive){e.preventDefault();searchText=searchText.slice(0,-1);return;}
    if(k==="Escape"){e.preventDefault();searchActive=false;selectPage(0);return;}
    return;
  }
  if(page===2){ // SYS navigation
    if(k==="ArrowUp"){e.preventDefault();settingRow=(settingRow+4)%5;markRailActivity();return;}
    if(k==="ArrowDown"){e.preventDefault();settingRow=(settingRow+1)%5;markRailActivity();return;}
    if(k==="ArrowLeft"){e.preventDefault();adjustSetting(-1);return;}
    if(k==="ArrowRight"||k==="Enter"){e.preventDefault();adjustSetting(1);return;}
    if(k==="Escape"){e.preventDefault();selectPage(0);return;}
    if(k==="+"){e.preventDefault();adjustSetting(1);return;}
    if(k==="-"){e.preventDefault();adjustSetting(-1);return;}
    return;
  }
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
      stateView.setInt32(2664,page,true); // page: TR/LIB/SYS
      stateView.setInt32(2668,-1,true); // hoverChannel
      stateView.setFloat64(2672,seconds,true);
      stateView.setInt32(2680,current!==null&&current>=names.length?1:0,true);
      stateView.setInt32(2684,loading?1:0,true);
      // Keep the TR/LIB/SYS rail visible: it is the only on-screen page switch.
      stateView.setInt32(2688,tick,true);
      stateView.setInt32(2720,switchTick,true);
      stateView.setInt32(2724,showGridOverlay?1:0,true);
      stateView.setFloat64(2728,buffer?buffer.duration:0,true);
      // SYS / LIB shell fields (POD tail).
      stateView.setInt32(2736,lang,true);                    // language
      stateView.setInt32(2740,volume,true);                  // volume
      stateView.setInt32(2744,libFont,true);                 // libFont
      stateView.setInt32(2748,settingRow,true);              // settingRow
      // layoutName (2776..2839) as UTF-32
      const lname=LAYOUTS[layoutMode]||"ORIGINAL";
      for(let i=0;i<lname.length&&i<16;i++)stateView.setInt32(2776+i*4,lname.charCodeAt(i),true);
      stateView.setInt32(2840,lname.length,true);            // layoutNameLen
      // LIB page fields
      stateView.setInt32(2752,libCursor,true);
      stateView.setInt32(2756,libScroll,true);
      stateView.setInt32(2760,localOrder,true);
      stateView.setInt32(2764,paused?1:0,true);
      stateView.setInt32(2768,current!==null&&current>=names.length?1:0,true);
      stateView.setInt32(2772,Math.round(position()*1000),true);
      for(let i=0;i<searchText.length&&i<32;i++)stateView.setInt32(2844+i*4,searchText.charCodeAt(i),true);
      stateView.setInt32(2972,searchText.length,true);

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
  // Optional deep-link: #sys / #lib selects the initial page (handy for tests).
  const h=(location.hash||"").toLowerCase();
  if(h.includes("sys"))page=2;else if(h.includes("lib"))page=1;
  if(h.includes("settled"))tick=200; // skip entrance tint for deterministic capture
  loadFontForLang();
  // Test hook: exposes state and synthesized canvas clicks for headless checks.
  window.__hsTest={
    get page(){return page;},
    setPage(p){selectPage(p);},
    clickCanvas(x,y){const r=screen.getBoundingClientRect();screen.dispatchEvent(new MouseEvent("click",{clientX:r.left+x*(r.width/640),clientY:r.top+y*(r.height/480),bubbles:true}));}
  };
  // Deterministic click test via #testclick=<x>,<y> in logical canvas coords.
  const tc=(location.hash.match(/testclick=(\d+),(\d+)/)||[]);
  if(tc.length===3)setTimeout(()=>window.__hsTest.clickCanvas(+tc[1],+tc[2]),600);
  drawFrame();await request("init");ready=true;screen.focus();wakeAux();
}catch(e){
  document.title="SOUND MEMOIRS  [INIT FAILED]";
}
