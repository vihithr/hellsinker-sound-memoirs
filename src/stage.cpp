#include "stage.h"
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
void Memory::advance(const memoirs::Spectrum& s,int id,double seconds,bool running){
    if(id!=track || (running && last>=0 && (seconds+.10<last || seconds-last>1.0))) {
        *this=Memory{}; track=id;
    }
    if(!running || (last>=0 && seconds>=last && seconds-last<.045))return;
    last=seconds;
    head=(head+1)%24; trace=(trace+1)%6; samples=std::min(24,samples+1);
    for(int i=0;i<43;i++)ridges[head][i]=std::min(1.f,s.barsLevel[i]/96.f);
    for(int i=0;i<120;i++){
        float l=s.waveL[2*i],r=s.waveR[2*i];
        traces[trace][i]={LONG((l-r)*.92),LONG((l+r)*.72)};
    }
}
namespace {
using C=COLORREF;
constexpr double tau=6.283185307179586;
C mix(C a,C b,double t){
    auto f=[&](int x,int y){return int(x+(y-x)*t);};
    return RGB(f(GetRValue(a),GetRValue(b)),f(GetGValue(a),GetGValue(b)),f(GetBValue(a),GetBValue(b)));
}
struct Paper{
    HDC d;
    void fill(int x,int y,int w,int h,C c){RECT r{x,y,x+w,y+h};SetDCBrushColor(d,c);FillRect(d,&r,(HBRUSH)GetStockObject(DC_BRUSH));}
    void path(const POINT* p,int n,C c){SetDCPenColor(d,c);Polyline(d,p,n);}
    void rule(int x,int y,int u,int v,C c){POINT p[]={{x,y},{u,v}};path(p,2,c);}
    void disc(int x,int y,int r,C c){SetDCPenColor(d,c);SetDCBrushColor(d,c);Ellipse(d,x-r,y-r,x+r+1,y+r+1);}

};
POINT orbit(double a,double rad,double squash,double angle,int x,int y){
    double u=rad*cos(a),v=rad*squash*sin(a);
    return {LONG(std::lround(x+u*cos(angle)-v*sin(angle))),LONG(std::lround(y+u*sin(angle)+v*cos(angle)))};
}
void eclipse(Paper p,const Palette& c,const memoirs::Spectrum& s,double time){
    constexpr int cx=320,cy=157;
    for(int k=0;k<3;k++){
        POINT pts[97];const double tilt=(k-1)*.24;
        for(int j=0;j<=96;j++)pts[j]=orbit(tau*j/96,151-k*12,.23,tilt,cx,cy);
        p.path(pts,97,c.grid);
        auto q=orbit(time*(.23+k*.08)+k*2,151-k*12,.23,tilt,cx,cy);
        p.disc(q.x,q.y,2,c.accent);
    }
    p.disc(cx,cy,34,c.line);
    for(int y=-30;y<=30;y+=3){int w=int(sqrt(900-y*y));p.rule(cx-w,cy+y,cx+w,cy+y,c.field);}
    p.disc(cx+6,cy-2,26,c.field);
    for(int j=0;j<86;j++){
        double a=tau*j/86;
        int r=39+int(sqrt(std::min(1.f,s.barsLevel[j%43]/100.f))*25);
        auto v=orbit(a,36,1,0,cx,cy),w=orbit(a,r,1,0,cx,cy);
        p.rule(v.x,v.y,w.x,w.y,j%2?c.line:c.accent);
    }
}
void strata(Paper p,const Palette& c,const Memory& m){
    for(int r=0;r<24;r++){
        const auto& row=m.ridges[(m.head+1+r)%24];POINT q[45];
        int base=141+r*4, left=192-r*2, width=256+r*4;
        for(int k=0;k<43;k++){
            double u=k/42.;double contour=9*exp(-pow((u-.38)*5,2))+6*exp(-pow((u-.75)*8,2));
            q[k]={left+k*width/42,LONG(base-contour-row[k]*54)};
        }
        q[43]={q[42].x,236};q[44]={q[0].x,236};
        SetDCPenColor(p.d,c.field);SetDCBrushColor(p.d,c.field);Polygon(p.d,q,45);
        p.path(q,43,r==23?c.accent:mix(c.grid,c.line,r/23.));
    }
}
void phase(Paper p,const Palette& c,const Memory& m){
    constexpr int x=320,y=157;
    for(int j=-3;j<=3;j++)p.rule(x-161,y+j*19,x+161,y+j*19,c.grid);
    for(int j=-8;j<=8;j++)p.rule(x+j*19,y-70,x+j*19,y+70,c.grid);
    POINT frame[]={{x-76,y},{x,y-70},{x+76,y},{x,y+70},{x-76,y}};p.path(frame,5,c.frame);
    long peak=0;for(const auto& layer:m.traces)for(const POINT& v:layer)
        peak=std::max(peak,std::max(std::abs(v.x),std::abs(v.y)));
    const double gain=std::min(4.,68./std::max(1L,peak));
    for(int j=0;j<std::min(6,m.samples);j++){
        const auto& trace=m.traces[(m.trace+7-std::min(6,m.samples)+j)%6];POINT pts[120];
        for(int k=0;k<120;k++)pts[k]={x+LONG(trace[k].x*gain),y-LONG(trace[k].y*gain)};
        p.path(pts,120,j==std::min(6,m.samples)-1?c.accent:mix(c.grid,c.line,.22+.12*j));
    }
    p.fill(x-1,y-1,3,3,c.accent);
}
}
void paint(const Palette& c,HDC d,float scale,int mode,const memoirs::Spectrum& s,const Memory& m){
    int save=SaveDC(d);if(!save)return;
    SetMapMode(d,MM_ANISOTROPIC);SetWindowExtEx(d,640,480,nullptr);
    SetViewportExtEx(d,int(640*scale+.5f),int(480*scale+.5f),nullptr);
    IntersectClipRect(d,148,80,492,238);SelectObject(d,GetStockObject(DC_PEN));SelectObject(d,GetStockObject(DC_BRUSH));
    Paper p{d};p.fill(148,80,344,158,c.field);
    if(mode==1)eclipse(p,c,s,std::max(0.,m.last));else if(mode==2)strata(p,c,m);else phase(p,c,m);
    p.fill(148,236,344,2,c.frame);
    RestoreDC(d,save);
}
}
