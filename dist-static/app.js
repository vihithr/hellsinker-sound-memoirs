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
// One obvious transport button: start when idle, pause/resume while loaded.
function updatePlayButton(){
  const b=$("aux-play"); if(!b) return;
  b.textContent=(current!==null&&!paused)?"⏸":"▶";
}
$("aux-play")?.addEventListener("click",()=>{
  if(buffer&&current!==null){
    togglePause();
  }else if(!canPlay(selected)){
    lastError=locals.length?"SELECT A LIB TRACK":"LOAD A PAK OR MUSIC FILE";
  }else{
    activate();
  }
  updatePlayButton();wakeAux();
});
// Prev/next transport: walks the LIB list when a local track is in play,
// otherwise steps through the TR jukebox.
function nextTrack(delta){
  const inLib=page===1||(current!==null&&current>=names.length);
  if(inLib&&visible.length){stepLocal(delta);return;}
  if(!names.length){lastError="LOAD A PAK OR MUSIC FILE";return;}
  let idx=(current!==null&&current>=0&&current<names.length)?current:selected;
  if(idx<0)idx=0;
  idx=(idx+delta+names.length)%names.length;
  select(idx);activate();
}
$("aux-prev")?.addEventListener("click",()=>{nextTrack(-1);wakeAux();});
$("aux-next")?.addEventListener("click",()=>{nextTrack(1);wakeAux();});
$("aux-full")?.addEventListener("click",toggleFullscreen);
$("aux-page")?.addEventListener("click",()=>{selectPage((page+1)%3);wakeAux();});
$("aux-theme")?.addEventListener("click",()=>{cycleTheme();wakeAux();});
$("aux-stage")?.addEventListener("click",()=>{cycleStage();wakeAux();});
$("aux-wing")?.addEventListener("click",()=>{cycleWing();wakeAux();});
$("aux-layout")?.addEventListener("click",()=>{cycleLayout();wakeAux();});
$("aux-help")?.addEventListener("click",()=>{
  const h=$("help");h.classList.remove("show");void h.offsetWidth;h.classList.add("show");wakeAux();
});

// ---- state (mirrors the desktop PlayerApp subset the replica needs) --------
let names=[],locals=[],available=[],selected=0,current=null;
let context=null,buffer=null,source=null,gain=null,vizCore=null,vizPtr=0,statePtr=0,stateView=null;
let offset=0,started=0,epoch=0,loading=false,theme=0,volume=80,stageMode=0,wingMode=0,layoutMode=0;
let tick=0,paused=false,railActivityTick=0,switchTick=0,showGridOverlay=false;
let page=0; // 0=TR, 1=LIB, 2=SYS
let settingRow=0,libFont=1;
let libCursor=0,libScroll=0,searchText="",searchActive=false;
let visible=[];  // indices into `locals` after the search filter
function clampLibScroll(){if(libCursor<libScroll)libScroll=libCursor;if(libCursor>=libScroll+7)libScroll=libCursor-6;libScroll=Math.max(0,libScroll);}
// Recompute the filtered LIB list and push it into the shared painter.
function refreshVisible(){
  const q=searchText.trim().toLowerCase();
  visible=[];
  for(let i=0;i<locals.length;i++) if(!q||locals[i].toLowerCase().includes(q)) visible.push(i);
  if(libCursor>=visible.length) libCursor=Math.max(0,visible.length-1);
  clampLibScroll();
  pushLocalNames();
}
// Upload the visible names as fixed-width ASCII records for the C++ painter.
function pushLocalNames(){
  if(!vizCore||!vizCore._scene_set_local_names) return;
  const W=vizCore._scene_local_name_width?vizCore._scene_local_name_width():96;
  const max=vizCore._scene_local_name_max?vizCore._scene_local_name_max():64;
  const count=Math.min(visible.length,max);
  const bytes=new Uint8Array(W*(count||1));
  const enc=new TextEncoder();  // UTF-8 so CJK file names survive
  for(let i=0;i<count;i++){
    const name=locals[visible[i]]||"";
    const utf8=enc.encode(name);
    // Copy whole code points only; the C++ side re-checks the boundary too.
    let n=0;
    while(n<utf8.length&&n<W-1){
      const b=utf8[n];
      const len=(b<0x80)?1:(b>=0xF0)?4:(b>=0xE0)?3:(b>=0xC0)?2:1;
      if(n+len>W-1) break;
      n+=len;
    }
    bytes.set(utf8.subarray(0,n),i*W);
  }
  const ptr=vizCore._alloc(bytes.length);
  if(!ptr) return;
  vizCore.HEAPU8.set(bytes,ptr);
  vizCore._scene_set_local_names(ptr,count);
  vizCore._dealloc(ptr);
}
// Play the highlighted LIB row (cursor indexes the filtered list).
function libPlayCursor(){
  if(!visible.length) return;
  const real=visible[libCursor];
  if(real==null) return;
  localIndex=real;
  selected=names.length+real;
  activate();
}
// A local track finished: advance according to the LIB order mode, mirroring
// the desktop OnTimer check (sequential / repeat-one / repeat-all / shuffle).
function onLocalTrackEnded(){
  const count=visible.length;
  if(!count)return;
  if(localOrder===1){          // ONE: replay the same track
    libPlayCursor();
    return;
  }
  let row=libCursor;
  if(localOrder===3&&count>1){ // SHUFFLE: any other row
    let n=row;
    while(n===row) n=Math.floor(Math.random()*count);
    row=n;
  }else{
    row+=1;
    if(row>=count){
      if(localOrder===2) row=0;   // LOOP: wrap to the first row
      else return;                // ORDER: stop at the end of the list
    }
  }
  libCursor=Math.max(0,Math.min(count-1,row));
  clampLibScroll();
  libPlayCursor();
}
let lang=0; // 0=EN, 1=ZH, 2=JA
let playlist=[],localIndex=-1,localOrder=0; // 0 sequential, 1 repeat-one, 2 repeat-all, 3 shuffle
const localFiles=new Map();  // norm(name) -> File, for browser-native decoding
const fileKey=s=>s.toLowerCase().replaceAll("/","\\");
let scrubbing=false,scrubFraction=0,next=0,fatal=null,ready=false,lastError="";
const THEMES=["ORIGINAL","DARK","NEON","PAPER","BLUEPRINT"];
const LAYOUTS=["ORIGINAL","FIELD","STACK","RING","CROSS","SINK","DIVE","SONO","TRACK","DECK"];
const ORDERS=["ORDER","ONE","LOOP","SHUFFLE"];
const pending=new Map(),worker=new Worker("./worker.js",{type:"module"});

// ---- worker RPC -----------------------------------------------------------
function failWorker(message){fatal=message;for(const p of pending.values())p.reject(Error(message));pending.clear();}
worker.onerror=e=>failWorker(e.message||"无法启动 WASM 核心");
worker.onmessage=({data})=>{const p=pending.get(data.id);if(!p)return;pending.delete(data.id);data.error?p.reject(Error(data.error)):p.resolve(data);};
function request(op,args={}){return new Promise((resolve,reject)=>{
  if(fatal){reject(Error(fatal));return;}const id=++next;pending.set(id,{resolve,reject});worker.postMessage({id,op,...args});});}

// ---- title / chrome -------------------------------------------------------
// Side-rail / help chrome follows the active theme (values from palettes.json).
const CHROME=[
  {bg:"rgba(20,28,26,.88)",border:"#84e6e0",btn:"rgba(36,48,44,.85)",hover:"rgba(60,80,72,.95)",ink:"#e8f2ee",accent:"#e87828",halo:"rgba(132,230,224,.10)"}, // ORIGINAL
  {bg:"rgba(10,13,18,.92)",border:"#3cdcd2",btn:"rgba(24,28,34,.9)",hover:"rgba(44,52,64,.95)",ink:"#dce1e6",accent:"#ff783c",halo:"rgba(60,220,210,.08)"},  // DARK
  {bg:"rgba(8,10,18,.92)",border:"#2e48a8",btn:"rgba(14,18,30,.9)",hover:"rgba(30,26,54,.95)",ink:"#e6f0ff",accent:"#ff2e97",halo:"rgba(255,46,151,.10)"},  // NEON
  {bg:"rgba(238,232,218,.94)",border:"#5a6e64",btn:"rgba(222,216,202,.9)",hover:"rgba(200,192,172,.95)",ink:"#32302c",accent:"#aa3c1e",halo:"rgba(246,242,232,.12)"}, // PAPER
  {bg:"rgba(16,32,50,.92)",border:"#88c8d8",btn:"rgba(25,53,79,.9)",hover:"rgba(42,74,104,.95)",ink:"#e6eff1",accent:"#f2c184",halo:"rgba(136,200,216,.10)"}, // BLUEPRINT
];
function applyChrome(){
  const c=CHROME[theme]||CHROME[0],r=document.documentElement.style;
  r.setProperty("--chrome-bg",c.bg);r.setProperty("--chrome-border",c.border);
  r.setProperty("--chrome-btn",c.btn);r.setProperty("--chrome-btn-hover",c.hover);
  r.setProperty("--chrome-ink",c.ink);r.setProperty("--chrome-accent",c.accent);
  r.setProperty("--halo",c.halo);
}
function refreshTitle(){
  const t=THEMES[theme];
  document.title=`SOUND MEMOIRS  [VOL ${volume}] [${t}]`+(loading?"  [LOADING]":"");
  applyChrome();
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
  s.onended=()=>{
    if(source!==s)return;
    source=null;offset=0;
    // Local tracks advance according to the LIB order mode; TR tracks loop
    // inside the source itself and never reach here.
    if(current!==null&&current>=names.length)onLocalTrackEnded();
  };
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
    const {pcm,native}=await request("decode",{key:target<names.length?{name:names[target]}:{local:locals[target-names.length]}});
    if(id!==epoch)return;
    if(native){
      // MP3/FLAC/M4A/... : decode with the platform decoder, then normalize
      // to 44.1 kHz stereo so the shared spectrum/visual-cursor code keeps
      // its desktop-identical timing assumptions.
      const key=fileKey(locals[target-names.length]);
      const file=localFiles.get(key);
      if(!file)throw Error("找不到本地文件");
      buffer=await decodeBrowser(file);
      lastError="";
      current=target;loading=false;refreshTitle();start();
      return;
    }
    const b=context.createBuffer(2,pcm.length/2,44100);
    for(let c=0;c<2;c++){const out=b.getChannelData(c);for(let i=0;i<out.length;i++)out[i]=pcm[2*i+c]/32768;}
    buffer=b;lastError="";current=target;loading=false;refreshTitle();start();
  }catch(e){
    // The on-canvas hint is ASCII-only (the atlas has no CJK glyphs).
    console.warn(e);
    lastError="PLAYBACK FAILED";
    if(id===epoch){reset();}
  }
}
function seek(seconds){if(!buffer)return;offset=Math.max(0,Math.min(buffer.duration,seconds));if(source)start();}
// Decode a browser-supported container (MP3/AAC/M4A/FLAC/Opus/...) and
// normalize it to 44.1 kHz stereo, the format the shared C++ spectrum and
// visual cursor assume.
async function decodeBrowser(file){
  const bytes=await file.arrayBuffer();
  let decoded;
  try{
    // Chrome/Safari need a copy here: decodeAudioData detaches its input.
    decoded=await context.decodeAudioData(bytes.slice(0));
  }catch(e){
    throw Error("浏览器无法解码此格式（"+file.name+"）");
  }
  if(!decoded||!decoded.length) throw Error("解码结果为空："+file.name);
  const OfflineCtx=window.OfflineAudioContext||window.webkitOfflineAudioContext;
  if(decoded.sampleRate===44100&&decoded.numberOfChannels>=2) return decoded;
  if(!OfflineCtx) return decoded;  // fall back: the cursor is sample-rate aware
  const frames=Math.ceil(decoded.duration*44100);
  if(frames<1) return decoded;
  const off=new OfflineCtx(2,frames,44100);
  const src=off.createBufferSource();
  src.buffer=decoded;src.connect(off.destination);src.start();
  return await off.startRendering();
}
// Quick pause/resume for touch users: toggles whatever is currently playing.
function togglePause(){
  if(current===null||!buffer)return false;
  if(!paused){offset=position();detach();paused=true;}
  else{paused=false;start();}
  refreshTitle();markRailActivity();
  return true;
}
function setVolume(v){
  volume=Math.max(0,Math.min(100,v));
  if(gain)gain.gain.value=volume/100;
  refreshTitle();markRailActivity();
}
function stop(){reset();}
function stepLocal(delta){
  const count=visible.length;if(!count)return;
  // Move within the filtered list, then map back to the real `locals` index.
  let row=libCursor+delta;
  if(localOrder===2)row=(row+count)%count;
  else if(localOrder===3)row=count<2?0:(row+count)%count;
  if(row<0||row>=count)return;
  libCursor=row;clampLibScroll();
  const real=visible[row];
  localIndex=real;selected=names.length+real;activate();
}
function cycleTheme(){theme=(theme+1)%THEMES.length;refreshTitle();markRailActivity();}
function cycleStage(){stageMode=(stageMode+1)%4;markRailActivity();}
function cycleWing(){wingMode=(wingMode+1)%5;markRailActivity();}
function cycleLayout(){layoutMode=(layoutMode+1)%LAYOUTS.length;showGridOverlay=false;markRailActivity();}
function selectPage(p){page=p;switchTick=tick;document.body.dataset.page=String(p);markRailActivity();}
function adjustSetting(direction){
  switch(settingRow){
    case 0: setVolume(volume+direction*5); break;
    case 1: theme=(theme+(direction>0?1:4))%5; refreshTitle(); break;
    case 2: lang=(lang+(direction>0?1:2))%3; loadFontForLang(); refreshTitle(); break;
    case 3: libFont=libFont?0:1; break;
    case 4: layoutMode=(layoutMode+(direction>0?1:LAYOUTS.length-1))%LAYOUTS.length;showGridOverlay=false; break;
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
// Hit tests for the custom compositions (page 0). Returns a track index,
// "exit", or null. Mirrors HandleLayoutMouse in the desktop shell.
function hitLayout(x,y){
  if(layoutMode===2){ // STACK: compact right index, two columns of 23
    if(x>=460&&x<620&&y>=58&&y<426){
      const i=Math.floor((x-460)/80)*23+Math.floor((y-58)/16);
      if(i>=0&&i<names.length)return i;
    }
    return null;
  }
  if(layoutMode===3){ // RING: outer tick ring selects a track
    const dx=x-320,dy=y-236,r2=dx*dx+dy*dy;
    if(r2>=170*170&&r2<=230*230){
      let a=Math.atan2(dy,dx)+Math.PI/2;if(a<0)a+=2*Math.PI;
      return Math.round(a/(2*Math.PI)*names.length)%names.length;
    }
    return null;
  }
  if(layoutMode===4){ // CROSS: EXIT label in the bottom-right transport bay
    if(x>=432&&x<480&&y>=344&&y<362)return "exit";
    return null;
  }
  return null; // FIELD / HELIX use the TAB grid / keyboard
}
screen.addEventListener("click",e=>{
  const {x,y}=canvasPoint(e);
  // Channel rail (TR/LIB/SYS) sits at x 362..486, y 272..288 on every page.
  if((page!==0||layoutMode===0)&&y>=272&&y<288&&x>=366&&x<486){
    const ch=Math.floor((x-366)/40);
    if(ch>=0&&ch<3){selectPage(ch);return;}
  }
  if(page===1){ // LIB
    if(y>=293&&y<309){files.click();return;}
    if(y>=313&&y<439){const row=Math.floor((y-313)/18);const idx=libScroll+row;if(idx>=0&&idx<visible.length){libCursor=idx;markRailActivity();}return;}
    if(y>=443&&y<460){
      if(x>=154&&x<212){localOrder=(localOrder+1)%4;markRailActivity();return;}
      if(x>=213&&x<277){togglePause();return;}
      if(x>=278&&x<331){stop();return;}
      if(x>=341&&x<375){libCursor=Math.max(0,libCursor-7);clampLibScroll();return;}
      if(x>=453&&x<486){libCursor=Math.min(Math.max(0,visible.length-1),libCursor+7);clampLibScroll();return;}
    }
    return;
  }
  if(page===2){ // SYS
    if(y>=283&&y<438){const row=Math.floor((y-283)/31);if(row>=0&&row<5){settingRow=row;if(x>=269&&x<296)adjustSetting(-1);else if(x>=462&&x<486)adjustSetting(1);markRailActivity();}return;}
    return;
  }
  if(layoutMode!==0){
    if(showGridOverlay){
      if(hitExit(x,y)){stop();return;}
      const i=hitTrack(x,y);if(i!==null){select(i);activate();}
      return;
    }
    const h=hitLayout(x,y);
    if(h==="exit"){stop();return;}
    if(h!==null){select(h);activate();}
    return;
  }
  if(hitExit(x,y)){stop();return;}
  const i=hitTrack(x,y);if(i!==null){select(i);activate();}
});
screen.addEventListener("dblclick",e=>{
  const {x,y}=canvasPoint(e);
  // Double-click a LIB row to play it (single click only moves the cursor).
  if(page===1&&y>=313&&y<439){
    const row=Math.floor((y-313)/18),idx=libScroll+row;
    if(idx>=0&&idx<visible.length){libCursor=idx;markRailActivity();libPlayCursor();}
    return;
  }
});
// ---- scrubbable seek rail (LIB page, y 266..268) --------------------------
let scrub=null;
function updateScrub(x){
  const fraction=Math.max(0,Math.min(1,(x-scrub.left)/(scrub.right-scrub.left)));
  scrub.seconds=fraction*scrub.buffer.duration;
}
screen.addEventListener("pointerdown",e=>{
  if(scrub||!buffer||e.button!==0)return;
  const {x,y}=canvasPoint(e);
  const library=page===1&&x>=150&&x<=490&&y>=258&&y<=278;
  const footer=page===0&&layoutMode!==0&&!showGridOverlay&&x>=10&&x<=630&&y>=448&&y<=462;
  if(!library&&!footer)return;
  scrub={id:e.pointerId,buffer,left:library?154:16,right:library?486:624,seconds:0};
  updateScrub(x);
  screen.setPointerCapture(e.pointerId);
  e.preventDefault();
});
screen.addEventListener("pointermove",e=>{
  if(scrub?.id!==e.pointerId)return;
  updateScrub(canvasPoint(e).x);
  e.preventDefault();
});
screen.addEventListener("pointerup",e=>{
  if(scrub?.id!==e.pointerId)return;
  updateScrub(canvasPoint(e).x);
  if(scrub.buffer===buffer)seek(scrub.seconds);
  scrub=null;
});
screen.addEventListener("lostpointercapture",e=>{
  if(scrub?.id===e.pointerId)scrub=null;
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
  if(page===1){ // LIB navigation (operates on the filtered `visible` list)
    const n=visible.length;
    if(k==="ArrowUp"){e.preventDefault();libCursor=Math.max(0,libCursor-1);clampLibScroll();markRailActivity();return;}
    if(k==="ArrowDown"){e.preventDefault();libCursor=Math.min(Math.max(0,n-1),libCursor+1);clampLibScroll();markRailActivity();return;}
    if(k==="PageUp"){e.preventDefault();libCursor=Math.max(0,libCursor-7);clampLibScroll();return;}
    if(k==="PageDown"){e.preventDefault();libCursor=Math.min(Math.max(0,n-1),libCursor+7);clampLibScroll();return;}
    if(k==="Enter"){e.preventDefault();if(n){libPlayCursor();}return;}
    if(k===" "){e.preventDefault();togglePause();return;}
    if(k==="ArrowLeft"||k==="ArrowRight"){e.preventDefault();seek(position()+(k==="ArrowLeft"?-5:5));return;}
    if(k==="/"){e.preventDefault();searchText="";searchActive=true;refreshVisible();return;}
    if(searchActive&&k.length===1){searchText+=k;refreshVisible();markRailActivity();return;}
    if(k==="Backspace"&&searchActive){e.preventDefault();searchText=searchText.slice(0,-1);refreshVisible();return;}
    if(k==="Escape"){e.preventDefault();searchActive=false;refreshVisible();selectPage(0);return;}
    return;
  }
  if(page===2){ // SYS navigation
    if(k==="c"||k==="C"||k==="t"||k==="T"){cycleTheme();return;}
    if(k==="F9"){e.preventDefault();cycleLayout();return;}
    if(k==="F7"){e.preventDefault();cycleStage();return;}
    if(k==="F8"){e.preventDefault();cycleWing();return;}
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
  if(k==="Tab"&&page===0&&(layoutMode===1||layoutMode===4)){e.preventDefault();showGridOverlay=!showGridOverlay;return;}
  if(k==="Escape"){stop();return;}
});
async function importFiles(picked){
  if(!picked||!picked.length)return;
  loading=true;refreshTitle();
  // Keep the File handles on the main thread: browser-native formats are
  // decoded here (the worker has no AudioContext).
  for(const f of picked) localFiles.set(fileKey(f.name), f);
  try{
    const result=await request("import",{files:picked,names});
    available=result.available;locals=result.locals||[];playlist=locals.slice();
    refreshVisible();
    if(!canPlay(selected)){
      const i=available.findIndex(Boolean);
      selected=i>=0?i:(locals.length?names.length+visible[0]:0);
    }
    localIndex=locals.length?visible[0]:-1;
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
    // Always re-arm first: any exception below must not kill the render loop
    // (a dead rAF chain freezes the whole player on the last painted frame).
    requestAnimationFrame(drawFrame);
    try{
    if(now-last>=1000/60-.5){
      last=now;tick++;
      const seconds=position();
      const playing=!!source&&!paused;
      if(playing&&buffer){
        // Same 125 ms visual-cursor compensation as the desktop, but expressed
        // in this buffer's own sample rate: a 48 kHz or mono MP3 would otherwise
        // desync the analyser from the audio.
        const rate=buffer.sampleRate;
        const n=buffer.length;
        const first=Math.floor(Math.abs(Math.floor(seconds*rate)*4-Math.round(rate*0.5))/4)%n;
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
      const shownSeconds=scrub&&scrub.buffer===buffer?scrub.seconds:seconds;
      stateView.setFloat64(2672,shownSeconds,true);
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
      stateView.setInt32(2772,Math.round(shownSeconds*1000),true);
      for(let i=0;i<searchText.length&&i<32;i++)stateView.setInt32(2844+i*4,searchText.charCodeAt(i),true);
      stateView.setInt32(2972,searchText.length,true);
      // Row index (within the filtered list) of the playing local track.
      stateView.setInt32(2976,current!==null&&current>=names.length?visible.indexOf(current-names.length):-1,true);

      vizCore._scene_begin();
      vizCore._scene_set_state(statePtr);
      vizCore._scene_advance(current===null?-1:current,seconds,playing?1:0,paused?1:0);
      vizCore._scene_render();
      // On-canvas status line: the transport state must be visible without
      // reading a help panel, otherwise "nothing happens" looks like a bug.
      const hasAny=available.some(Boolean)||locals.length>0;
      let hint="";
      if(loading) hint="DECODING ...";
      else if(lastError) hint=lastError;
      else if(!hasAny) hint="PRESS + TO LOAD MUSIC";
      else if(current===null) hint="PRESS PLAY TO START";
      draw({hintTheme:theme,hint});
      updatePlayButton();
    }
    }catch(e){
      // Surface the failure once instead of freezing silently.
      if(!window.__drawErr){window.__drawErr=String(e&&e.stack||e);console.error("drawFrame:",e);}
    }
  };
  // Optional deep-link: #sys / #lib selects the initial page (handy for tests).
  const h=(location.hash||"").toLowerCase();
  if(h.includes("sys"))page=2;else if(h.includes("lib"))page=1;
  if(h.includes("settled"))tick=200; // skip entrance tint for deterministic capture
  loadFontForLang();
  refreshVisible();   // seed the C++ LIB name pool (empty until files load)
  // Test hook: exposes state and synthesized canvas clicks for headless checks.
  window.__hsTest={
    get page(){return page;},
    setPage(p){selectPage(p);},
    get lang(){return lang;},
    get settingRow(){return settingRow;},
    get layout(){return layoutMode;},
    get theme(){return theme;},
    get stage(){return stageMode;},
    get wing(){return wingMode;},
    get libFont(){return libFont;},
    get visible(){return visible.slice();},
    get libNameCount(){return visible.length;},
    setLocals(list){locals=list.slice();playlist=locals.slice();refreshVisible();},
    get playing(){return !!source&&!paused;},
    get paused(){return paused;},
    get positionMs(){return Math.round(position()*1000);},
    get currentRow(){return current!==null&&current>=names.length?visible.indexOf(current-names.length):-1;},
    get selected(){return selected;},
    get current(){return current;},
    get localOrder(){return localOrder;},
    set localOrder(v){localOrder=v;},
    libPlay(row){libCursor=Math.max(0,Math.min(visible.length-1,row));clampLibScroll();libPlayCursor();},
    stopAll(){stop();},
    get bufferDuration(){return buffer?buffer.duration:0;},
    get localFilesSize(){return localFiles.size;},
    get lastError(){return lastError;},
    // Code points of every text run the shared painter emitted this frame
    // (used to verify CJK names actually reach the renderer).
    drawnText(){
      const n=vizCore._scene_text_count();
      const tp=vizCore._scene_texts()>>>0, pp=vizCore._scene_text_points()>>>0;
      const dv=new DataView(vizCore.HEAPU8.buffer,vizCore.HEAPU8.byteOffset+tp,n*20);
      const pdv=new DataView(vizCore.HEAPU8.buffer,vizCore.HEAPU8.byteOffset+pp);
      const out=[];
      for(let i=0;i<n;i++){
        const off=dv.getUint16(i*20,true),cnt=dv.getUint16(i*20+2,true);
        let s="";
        for(let k=0;k<cnt;k++) s+=String.fromCodePoint(pdv.getUint32((off+k)*4,true));
        out.push(s);
      }
      return out;
    },
    clickCanvas(x,y){const r=screen.getBoundingClientRect();screen.dispatchEvent(new MouseEvent("click",{clientX:r.left+x*(r.width/640),clientY:r.top+y*(r.height/480),bubbles:true}));}
  };
  // Deterministic click test via #testclick=<x>,<y> in logical canvas coords.
  const tc=(location.hash.match(/testclick=(\d+),(\d+)/)||[]);
  if(tc.length===3)setTimeout(()=>window.__hsTest.clickCanvas(+tc[1],+tc[2]),600);
  drawFrame();await request("init");ready=true;screen.focus();wakeAux();
}catch(e){
  document.title="SOUND MEMOIRS  [INIT FAILED]";
}
