const glyphs="ABCDEFGHIJKLNMOPQRSTUVWXYZ 0123456789_-.:;&<>*+";
export async function renderer(canvas) {
  const ctx=canvas.getContext("2d");
  const [cells,title,axis]=await Promise.all(["cells","title","axis"].map(async name=>{
    const img=new Image(); img.src="assets/"+name+".png"; await img.decode();return img;
  }));
  ctx.imageSmoothingEnabled=false;
  function box(x,y,w,h,color){ctx.fillStyle=color;ctx.fillRect(x,y,w,h);}
  function line(str,x,y,row=0){
    for(const c of str){let i=glyphs.indexOf(c);if(i<0)i=26;
      ctx.drawImage(cells,8*i,row*11,8,11,x-4,y-5,8,11);x+=8;}
  }
  return function draw({selected,current,available,seconds,visual}){
    box(0,0,640,480,"#d9e3e0");
    if(visual)for(const [side,x] of [[0,4],[1,502]]){
      const env=visual[609+side];
      for(let j=0;j<=Math.min(60,Math.floor(env*90))&&env>0;j++)
        box(x,473-8*j,134,6,j>44?"#bfdfe4":"#c3d9cb");
      ctx.beginPath();for(let j=0;j<240;j++){
        const dx=visual[129+side*240+j];
        if(j)ctx.lineTo(x+66+dx,j*2);else ctx.moveTo(x+66+dx,0);}
      ctx.strokeStyle="#97adbe";ctx.stroke();
    }
    ctx.strokeStyle="#84e6e0";ctx.strokeRect(142.5,4.5,354,471);
    ctx.drawImage(title,152,18,209,28);
    box(148,102,344,134,"#d8dcd0");
    for(const x of [171,195,243,291,339,387,435,475])box(x,102,1,134,"#d9ded9");
    if(visual)for(let i=0;i<43;i++){
      const level=Math.floor(visual[86+i]/4),tail=Math.floor(visual[43+i]/4);
      for(let j=0;j<=Math.min(43,Math.max(level,tail));j++){
        if(j===0 && !level && !tail)continue;
        box(148+i*8,230-3*j,7,2,j>=level&&tail>level?"#d5b7a9":"#cc3219");
      }
    }
    ctx.drawImage(axis,148,236,344,12);
    const info=current===null?"<SILENCE>":current<46?"NOW PLAYING : TRACK "+String(current).padStart(2,"0"):"NOW PLAYING : LOCAL FILE";
    line(info,220,256,3);line("PLAY TIME   : "+Math.floor(seconds)+"SEC",220,264,3);
    box(216,260,Math.min(270,info.length*8),1,"#4fd2b9");box(216,268,152,1,"#4fd2b9");
    line("STOP",168,280);
    for(let i=0;i<46;i++){
      const x=156+Math.floor(i/10)*56,y=288+i%10*16;
      if(i===selected)box(x,y,56,16,"#c68e48");
      line("TR:"+String(i).padStart(2,"0"),x+12,y+8,i===selected?1:available[i]?0:2);
    }
    line("SOUND MEMOIRS : LOCAL PAK",144,472,2);
  };
}
