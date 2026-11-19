#include "stage.h"
#include "gfx.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace stage {
const wchar_t* name(int i) {
    static const wchar_t* names[]={L"Original",L"Occultation  /  星蚀",L"Strata  /  地层",L"Phase study  /  相位习作"};
    return names[i>=0 && i<count?i:0];
}
const wchar_t* wingName(int i){
    static const wchar_t* names[]={L"Auto / 跟随中央",L"Level / 原版电平",L"Fold / 折叠波形",L"Ribbon / 双线带谱",L"Matrix / 点阵"};
    return names[i>=0 && i<wingCount?i:0];
}
namespace {
float follow(float current,float target,float dt,double attack,double release){
    const double tau=target>current?attack:release;
    return current+(target-current)*float(1.0-std::exp(-dt/tau));
}
float smoothstep(float edge0,float edge1,float value){
    const float t=std::clamp((value-edge0)/(edge1-edge0),0.f,1.f);
    return t*t*(3.f-2.f*t);
}
float channelDrive(float env,const float* wave){
    double sum=0;
    for(int i=0;i<240;i+=4){const double v=wave[i]/64.0;sum+=v*v;}
    const float rms=float(std::sqrt(sum/60.0));
    return smoothstep(.02f,.78f,std::max(env*2.7f,rms*1.8f));
}
}
void Memory::advance(const memoirs::Spectrum& s,int id,double seconds,bool playing,bool paused){
    // standby-aware animation clocks survive resets (freeze, never jump)
    struct Clocks { double sink,dive,sono,track,deck; };
    const Clocks keep{sinkClock,diveClock,sonoClock,trackClock,deckClock};
    auto restore=[&](const Clocks& c){
        sinkClock=c.sink; diveClock=c.dive; sonoClock=c.sono;
        trackClock=c.track; deckClock=c.deck;
    };
    if(id!=track || (playing && motion>=0 && (seconds+.10<motion || seconds-motion>1.0))){
        *this=Memory{}; track=id; restore(keep);
    }
    if(!playing){
        // standby lock: keep the frozen scene and clocks, zero the energy
        *this=Memory{}; track=id; restore(keep);
        return;
    }
    if(paused)return;
    float dt=motion>=0?float(seconds-motion):1.f/60.f;
    if(dt<=0)return;
    dt=std::clamp(dt,.001f,.12f);
    motion=seconds;
    sinkClock+=dt*8.0;    // 8 rungs/s at 10px = uniform 80px/s descent
    diveClock+=dt*0.32;   // one tunnel layer born every 1/0.32 s
    sonoClock+=dt*15.0;   // SONO waterfall prints 15 rows/s
    trackClock+=dt*12.0;  // TRACK pattern scrolls 12 rows/s
    deckClock+=dt*2.2;    // DECK reels spin at a uniform 2.2 rad/s

    float low=0;
    for(int i=0;i<6;++i)low+=std::clamp(s.barsLevel[i]/96.f,0.f,1.f);
    low/=6.f;
    lowFloor=follow(lowFloor,low,dt,.18,1.1);
    lowSustain=follow(lowSustain,low,dt,.045,.75);
    const float attack=std::max(0.f,low-lowFloor)*1.55f+std::max(0.f,low-lastLow)*.85f;
    impact=follow(impact,std::clamp(attack,0.f,1.f),dt,.022,.24);
    lastLow=low;
    float high=0;
    for(int i=28;i<=42;++i)high+=std::clamp(s.barsLevel[i]/96.f,0.f,1.f);
    high/=15.f;
    highFloor=follow(highFloor,high,dt,.14,.80);
    const float hAttack=std::max(0.f,high-highFloor)*1.8f+std::max(0.f,high-lastHigh)*1.0f;
    highSpark=follow(highSpark,std::clamp(hAttack,0.f,1.f),dt,.015,.14);
    lastHigh=high;
    wing[0]=follow(wing[0],channelDrive(s.envL,s.waveL),dt,.030,.30);
    wing[1]=follow(wing[1],channelDrive(s.envR,s.waveR),dt,.030,.30);

    // XY auto-gain: follow the live trace peak, attack 120 ms / release 1.6 s.
    {
        float peak=1.f;
        for(int i=0;i<120;i++){
            peak=std::max(peak,std::abs(float(s.waveL[2*i]-s.waveR[2*i]))*.92f);
            peak=std::max(peak,std::abs(float(s.waveL[2*i]+s.waveR[2*i]))*.72f);
        }
        const float target=std::clamp(56.f/peak,.5f,4.f);
        xyGain=follow(xyGain,target,dt,.12,1.6);
    }

    // Stereo phase correlation over the live window (true telemetry).
    {
        double sumLR=0,sumLL=0,sumRR=0;
        for(int i=0;i<240;i+=2){
            const double l=s.waveL[i],r=s.waveR[i];
            sumLR+=l*r;sumLL+=l*l;sumRR+=r*r;
        }
        const float corr=sumLL*sumRR>1e-3?float(sumLR/std::sqrt(sumLL*sumRR)):0.f;
        correlation=follow(correlation,std::clamp(corr,-1.f,1.f),dt,.08,.22);
    }
    // 1 s clip latch per channel: envelope near full scale or a hot sample.
    {
        const float env[2]={s.envL,s.envR};
        const float* wv[2]={s.waveL,s.waveR};
        for(int c=0;c<2;c++){
            bool hot=env[c]>=0.94f;
            if(!hot)for(int i=0;i<240;i++)if(std::abs(wv[c][i])>=61.f){hot=true;break;}
            clipHold[c]=hot?1.0f:std::max(0.f,clipHold[c]-dt);
        }
    }

    // Slow per-channel peak marks for the HELIX rack rails.
    const float drive[2]={channelDrive(s.envL,s.waveL),channelDrive(s.envR,s.waveR)};
    for(int c=0;c<2;c++){
        if(drive[c]>=meterPeak[c]){meterPeak[c]=drive[c];meterAge[c]=0;}
        else{
            meterAge[c]++;
            if(meterAge[c]>48)meterPeak[c]=std::max(drive[c],meterPeak[c]-0.55f*dt);
        }
    }

    // HELIX / SINK: sample the per-channel drives into the rung history at
    // 8 Hz; a fresh low attack marks the rung as a beat. Also keep a signed
    // waveform sample (mean of a small window) for the SINK strand projection.
    if(histLast<0 || seconds-histLast>=1./8.){
        histLast=seconds;
        histHead=(histHead+1)%kHist;
        histL[histHead]=drive[0];
        histR[histHead]=drive[1];
        histBeat[histHead]=attack>0.30f?1:0;
        // Winding phase from drive energy + zero-crossing density: a window
        // mean of AC audio cancels to zero (the old idle-spin bug), while the
        // crossing rate tracks pitch. Silence freezes the phase completely.
        int zcL=0,zcR=0;
        for(int i=1;i<240;i++){
            if((s.waveL[i-1]<0)!=(s.waveL[i]<0))++zcL;
            if((s.waveR[i-1]<0)!=(s.waveR[i]<0))++zcR;
        }
        const float znL=std::clamp(zcL/120.f,0.f,1.f), znR=std::clamp(zcR/120.f,0.f,1.f);
        const float stepL=drive[0]<0.02f?0.f:drive[0]*(0.28f+0.95f*znL);
        const float stepR=drive[1]<0.02f?0.f:drive[1]*(0.28f+0.95f*znR);
        windL+=stepL;
        windR+=stepR;
        // antiphase pair: the two strands always cross into the # lattice
        histWaveL[histHead]=std::sin(windL);
        histWaveR[histHead]=-std::sin(windR);
    }

    // DIVE: freeze a birth snapshot whenever a new layer spawns (0.32/s on
    // the standby-aware dive clock, synced with the painter). Ring of 16.
    {
        const int b=int(std::floor(diveClock));
        if(b>diveLastBase){
            if(diveLastBase<0){
                // cold start (track switch / first play): pre-fill a base
                // energy so the first layers are not flat-zero trusses
                for(int i=0;i<kDiveSlots;i++)diveDrive[i]=0.25f;
            }
            diveLastBase=b;
            const int slot=b%kDiveSlots;
            diveDrive[slot]=std::clamp((wing[0]+wing[1])*0.5f,0.f,1.f);
            diveSpark[slot]=highSpark;
            divePhase[slot]=windL;
        }
    }

    // SONO: print quantised spectrogram rows on the 15 rows/s clock (catch
    // up after a long frame so the scroll stays gapless)
    if(sonoLast<0)sonoLast=sonoClock;
    for(int guard=0;sonoClock-sonoLast>=1.0 && guard<8;++guard){
        sonoLast+=1.0;
        for(int k=0;k<43;k++)
            sono[sonoHead][k]=std::uint8_t(std::clamp(bandLevel(s.barsLevel[k]),0.f,1.f)*255.f);
        sonoHead=(sonoHead+1)%kSonoRows;
    }
    // TRACK: print 8-channel hex rows on the 12 rows/s clock (band max)
    if(trackLast<0)trackLast=trackClock;
    for(int guard=0;trackClock-trackLast>=1.0 && guard<8;++guard){
        trackLast+=1.0;
        static const int lo[8]={0,3,7,12,18,24,30,37};
        static const int hi[8]={2,6,11,17,23,29,36,42};
        for(int c=0;c<8;c++){
            float m=0;
            for(int k=lo[c];k<=hi[c];k++)m=std::max(m,bandLevel(s.barsLevel[k]));
            trackHex[trackHead][c]=std::uint8_t(std::clamp(m,0.f,1.f)*255.f);
        }
        trackBeat[trackHead]=attack>0.30f?1:0;
        trackHead=(trackHead+1)%kTrackRows;
        trackRowCounter=(trackRowCounter+1)&0xFF;
    }

    if(last>=0 && seconds>=last && seconds-last<.045)return;
    last=seconds;
    head=(head+1)%24; trace=(trace+1)%6; samples=std::min(24,samples+1);
    for(int i=0;i<43;i++)ridges[head][i]=std::min(1.f,s.barsLevel[i]/96.f);
    for(int i=0;i<120;i++){
        float l=s.waveL[2*i],r=s.waveR[2*i];
        traces[trace][i]={std::int16_t((l-r)*.92),std::int16_t((l+r)*.72)};
    }
}
namespace {
using C=COLORREF;
constexpr double tau=6.283185307179586;
C mix(C a,C b,double t){
    auto f=[&](int x,int y){return int(x+(y-x)*t);};
    return RGB(f(GetRValue(a),GetRValue(b)),f(GetGValue(a),GetGValue(b)),f(GetBValue(a),GetBValue(b)));
}
// The paper receives logical 640x480 coordinates and scales them to device
// pixels through the gfx backend. This replaces the former MM_ANISOTROPIC
// map-mode transform that the HDC-based version relied on.
struct Paper{
    float sc;
    int S(int v) const { return (int)(v*sc+0.5f); }
    void fill(int x,int y,int w,int h,C c){gfx_rect(S(x),S(y),S(x+w),S(y+h),c);}
    void path(const GfxPoint* p,int n,C c){
        if(n<=0||n>512)return;
        GfxPoint q[512];
        for(int i=0;i<n;++i){q[i].x=std::int16_t(S(p[i].x));q[i].y=std::int16_t(S(p[i].y));}
        gfx_polyline(q,n,c,1);
    }
    void rule(int x,int y,int u,int v,C c){GfxPoint p[2]={{std::int16_t(x),std::int16_t(y)},{std::int16_t(u),std::int16_t(v)}};path(p,2,c);}
    void disc(int x,int y,int r,C c){gfx_ellipse(S(x-r),S(y-r),S(x+r+1),S(y+r+1),c);}
};
GfxPoint orbit(double a,double rad,double squash,double angle,int x,int y){
    double u=rad*cos(a),v=rad*squash*sin(a);
    return {std::int16_t(std::lround(x+u*cos(angle)-v*sin(angle))),
            std::int16_t(std::lround(y+u*sin(angle)+v*cos(angle)))};
}
void eclipse(Paper p,const Palette& c,const memoirs::Spectrum& s,const Memory& m){
    constexpr int cx=320,cy=157;
    const int ring=136+int(14*m.lowSustain+12*m.impact);
    for(int k=0;k<3;k++){
        GfxPoint pts[97];const double tilt=(k-1)*.24;
        for(int j=0;j<=96;j++)pts[j]=orbit(tau*j/96,ring-k*12,.23,tilt,cx,cy);
        p.path(pts,97,k==0&&m.impact>.08f?mix(c.grid,c.accent,.28+.36*m.impact):c.grid);
        if(m.lowSustain>.035f){
            auto q=orbit(std::max(0.,m.last)*(.23+k*.08)+k*2,ring-k*12,.23,tilt,cx,cy);
            p.disc(q.x,q.y,1+int(2*m.impact),mix(c.grid,c.accent,.32+.68*m.impact));
        }
    }
    const int core=30+int(5*m.lowSustain+6*m.impact);
    p.disc(cx,cy,core+4,c.line);
    for(int y=-core;y<=core;y+=3){int w=int(sqrt(std::max(0,core*core-y*y)));p.rule(cx-w,cy+y,cx+w,cy+y,c.field);}
    p.disc(cx+6,cy-2,std::max(18,core-8),c.field);
    const int inner=34+int(8*m.impact);
    for(int j=0;j<86;j++){
        double a=tau*j/86;
        const float local=std::sqrt(std::min(1.f,s.barsLevel[j%43]/100.f));
        int r=inner+4+int(local*24+7*m.impact);
        auto v=orbit(a,inner,1,0,cx,cy),w=orbit(a,r,1,0,cx,cy);
        p.rule(v.x,v.y,w.x,w.y,j%2?c.line:c.accent);
    }
}
void strata(Paper p,const Palette& c,const Memory& m){
    for(int r=0;r<24;r++){
        const auto& row=m.ridges[(m.head+1+r)%24];GfxPoint q[45];
        int base=141+r*4, left=192-r*2, width=256+r*4;
        for(int k=0;k<43;k++){
            double u=k/42.;double contour=9*exp(-pow((u-.38)*5,2))+6*exp(-pow((u-.75)*8,2));
            q[k]={std::int16_t(left+k*width/42),std::int16_t(base-contour-row[k]*54)};
        }
        q[43]={q[42].x,236};q[44]={q[0].x,236};
        GfxPoint scaled[45];
        for(int i=0;i<45;++i){scaled[i].x=std::int16_t(p.S(q[i].x));scaled[i].y=std::int16_t(p.S(q[i].y));}
        gfx_polygon(scaled,45,c.field);
        p.path(q,43,r==23?c.accent:mix(c.grid,c.line,r/23.));
    }
}
void phase(Paper p,const Palette& c,const Memory& m){
    constexpr int x=320,y=157;
    for(int j=-3;j<=3;j++)p.rule(x-161,y+j*19,x+161,y+j*19,c.grid);
    for(int j=-8;j<=8;j++)p.rule(x+j*19,y-70,x+j*19,y+70,c.grid);
    GfxPoint frame[]={{std::int16_t(x-76),std::int16_t(y)},{std::int16_t(x),std::int16_t(y-70)},{std::int16_t(x+76),std::int16_t(y)},{std::int16_t(x),std::int16_t(y+70)},{std::int16_t(x-76),std::int16_t(y)}};
    p.path(frame,5,c.frame);
    // Smoothed auto-gain (Memory::xyGain) instead of a per-frame hard
    // normalisation: quiet stereo still opens up, and the figure breathes
    // with the music rather than snapping scale on every transient.
    const double gain=m.xyGain;
    const int layers=std::min(4,m.samples);
    for(int j=0;j<layers;j++){
        const auto& trace=m.traces[(m.trace+7-layers+j)%6];GfxPoint pts[120];
        for(int k=0;k<120;k++){
            const int px=std::clamp(int(trace[k].x*gain),-72,72);
            const int py=std::clamp(int(trace[k].y*gain),-64,64);
            pts[k]={std::int16_t(x+px),std::int16_t(y-py)};
        }
        // clear age hierarchy: newest bright accent, older layers sink fast
        const C col=j==layers-1?c.accent
                   :j==layers-2?c.line
                   :mix(c.grid,c.line,.10+.14*j);
        p.path(pts,120,col);
    }
    p.fill(x-1,y-1,3,3,c.accent);
}
void calibration(Paper p,const Palette& c,bool drawInnerGrid){
    if(drawInnerGrid){
        const C dot=mix(c.field,c.grid,.32);
        const C guide=mix(c.field,c.grid,.13);
        for(int x=172;x<484;x+=48)p.rule(x,88,x,228,guide);
        for(int y=92;y<232;y+=32)p.rule(158,y,482,y,guide);
        for(int y=88;y<232;y+=12)
            for(int x=156;x<484;x+=12)p.fill(x,y,1,1,dot);
    }

    const C rail=mix(c.field,c.grid,.62);
    p.rule(154,84,154,232,rail);
    p.rule(486,84,486,232,rail);
    for(int y=88,i=0;y<232;y+=6,++i){
        const int length=i%5==0?6:2;
        p.rule(154,y,154+length,y,rail);
        p.rule(486-length,y,486,y,rail);
    }
    for(int x=160,x0=0;x<486;x+=8,++x0){
        const int length=x0%5==0?4:2;
        p.rule(x,84,x,84+length,rail);
        p.rule(x,232-length,x,232,rail);
    }
    for(int k=0;k<12;k++){
        p.rule(158+k,88,158,88,rail);
        p.rule(482-k,88,482,88,rail);
        p.rule(158+k,228,158,228,rail);
        p.rule(482-k,228,482,228,rail);
    }
    p.rule(158,88,158,100,rail);
    p.rule(482,88,482,100,rail);
    p.rule(158,216,158,228,rail);
    p.rule(482,216,482,228,rail);
}
}
void paint(const Palette& c,float scale,int mode,const memoirs::Spectrum& s,const Memory& m){
    auto S=[&](int v){return (int)(v*scale+0.5f);};
    gfx_push_clip(S(148),S(80),S(492),S(238));
    Paper p{scale};p.fill(148,80,344,158,c.field);
    // Phase draws its own 19 px scope grid: skip the calibration dot grid so
    // the two grids never moire over each other (frame + ticks stay).
    calibration(p,c,mode!=3);
    if(mode==1)eclipse(p,c,s,m);else if(mode==2)strata(p,c,m);else phase(p,c,m);
    p.fill(148,236,344,2,c.frame);
    gfx_pop_clip();
}
}
