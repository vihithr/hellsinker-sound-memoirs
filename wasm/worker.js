import factory from "./core/core.js";
const ready = factory();
const HEADER = 0x203a4;
const text = new TextDecoder();
const slots = ["bbll.pak","bbt3.pak","bbt2.pak","bbt.pak"];
const indexes = new Map(), loose = new Map();
// OGG/WAV are decoded by the shared C++ core (same path as the desktop).
// Everything else is handed to the browser's decodeAudioData on the main
// thread, which covers MP3/AAC/M4A/FLAC/Opus and friends for free.
const CORE_EXT = /\.(ogg|wav)$/;
const NATIVE_EXT = /\.(mp3|m4a|aac|flac|opus|oga|webm|weba|wma|aiff|aif|mp4)$/;
const norm = s => s.toLowerCase().replaceAll("/", "\\");
function locate(name) {
  name = norm(name);
  if(loose.has(name+".wav")) return loose.get(name+".wav");
  for(const slot of slots) {
    const pak=indexes.get(slot);
    if(!pak) continue;
    const entry=pak.entries.get("backboneteller\\"+name+".ogg") || pak.bases.get(name+".ogg");
    if(entry) return pak.file.slice(entry[0],entry[0]+entry[1]);
  }
  return loose.get(name+".ogg");
}
async function ingest(core,file) {
  const key=norm(file.name);
  if(!slots.includes(key)) {
    if(!CORE_EXT.test(key) && !NATIVE_EXT.test(key)) throw Error("不支持："+file.name);
    loose.set(key,file);return;
  }
  if(file.size>0xffffffff) throw Error("当前 PAK 格式不支持超过 4 GiB 的文件");
  const header=new Uint8Array(await file.slice(0,HEADER).arrayBuffer());
  const ptr=core._alloc(header.length);
  if(!ptr) throw Error("内存不足");
  try {
    core.HEAPU8.set(header,ptr);
    const n=core._parse_pak(ptr,header.length,file.size);
    if(!n) throw Error("无法读取 PAK："+file.name);
    const entries=new Map(),bases=new Map();
    for(let i=0;i<n;i++) {
      const start=core._entry_name(i), heap=core.HEAPU8;
      const name=norm(text.decode(heap.subarray(start,heap.indexOf(0,start))));
      const range=[core._entry_offset(i)>>>0,core._entry_length(i)>>>0];
      const base=name.split("\\").pop();
      if(!entries.has(name)) entries.set(name,range);
      if(!bases.has(base)) bases.set(base,range);
    }
    indexes.set(key,{file,entries,bases});
  } finally {core._dealloc(ptr);}
}
async function handle({id,op,files,names,key}) {
  try {
    const core=await ready;
    if(op==="init") return postMessage({id});
    if(op==="import") {
      const errors=[];
      for(const f of files) try {await ingest(core,f);}catch(e){errors.push(e.message);}
      return postMessage({id,available:names.map(n=>!!locate(n)),locals:[...loose.keys()],errors});
    }
    const file=key.local?loose.get(key.local):locate(key.name);
    if(!file) throw Error("未载入此曲目");
    // Leave browser-native formats (MP3/FLAC/M4A/...) to the main thread,
    // which has an AudioContext. The worker only owns the C++ decoder.
    const fileName=norm(key.local||key.name||"");
    if(key.local && NATIVE_EXT.test(fileName)) {
      return postMessage({id,native:true,local:key.local});
    }
    if(file.size>96*1024*1024) throw Error("当前版本单曲输入上限 96 MiB");
    const bytes=new Uint8Array(await file.arrayBuffer()), ptr=core._alloc(bytes.length);
    if(!ptr) throw Error("内存不足");
    try {
      core.HEAPU8.set(bytes,ptr);
      const frames=core._decode(ptr,bytes.length);
      if(!frames) throw Error("无法解码此曲目");
      const first=core._samples()>>>1;
      const pcm=core.HEAP16.slice(first,first+2*frames);
      postMessage({id,pcm},[pcm.buffer]);
    } finally {core._clear_pcm();core._dealloc(ptr);}
  } catch(e){postMessage({id,error:String(e.message||e)});}
}
let queue=Promise.resolve();
onmessage=e=>{queue=queue.then(()=>handle(e.data));};
