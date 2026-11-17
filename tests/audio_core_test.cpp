#include "decoder.h"
#include "spectrum.h"
#include <algorithm>
#include <iostream>

int main() {
    // Two frames of little endian, 16-bit mono PCM at 22.05 kHz.
    const unsigned char wav[] = {
      'R','I','F','F',40,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,
      1,0,1,0,0x22,0x56,0,0,0x44,0xac,0,0,2,0,16,0,'d','a','t','a',4,0,0,0,
      0,0,0,64 };
    PcmBuffer pcm; std::string err;
    if (!DecodeWav(wav,sizeof(wav),pcm,err,0)) { std::cerr<<err;return 1; }
    if(pcm.sampleRate!=44100 || pcm.frames()!=4 || pcm.samples[2]!=8192 ||
       pcm.samples[3]!=8192 || pcm.samples[6]!=16384) return 2;
    short signal[2048]{};
    for(int i=0;i<1024;i++)signal[2*i+1]=(i%32<16?16384:-16384);
    memoirs::Spectrum viz;
    memoirs::analyze(viz,signal,1024,0);
    if(viz.envL<=0 || viz.envR!=0 || viz.waveL[0]!=32 || viz.waveR[0]!=0 ||
       *std::max_element(viz.barsLevel,viz.barsLevel+43)<=0) return 3;
    memoirs::analyze(viz,nullptr,0,0);
    if(viz.envL!=0 || viz.waveL[0]!=0) return 4;
    if(memoirs::visualCursor(0,44100,10000)!=5512) return 5;
}
