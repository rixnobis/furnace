// Ours. Headless positive/negative control for DivPlatformPS1.
// Instantiates the platform directly with a bare DivEngine (never init()ed) as parent,
// holding one 44100 Hz 1 kHz sine sample and one Amiga-type instrument.
// usage: ps1spu_test <on|off> <out.wav>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include "../../../engine.h"
#include "../../ps1.h"

// normally defined in main.cpp, which this harness replaces.
void reportError(String what) {
  logE("%s",what);
}

static void writeWav(const char* path, const std::vector<short>& l, const std::vector<short>& r, int rate) {
  FILE* f=fopen(path,"wb");
  if (!f) return;
  unsigned int n=l.size();
  unsigned int dataLen=n*4;
  unsigned int u32;
  unsigned short u16;
  fwrite("RIFF",1,4,f); u32=36+dataLen; fwrite(&u32,4,1,f);
  fwrite("WAVEfmt ",1,8,f); u32=16; fwrite(&u32,4,1,f);
  u16=1; fwrite(&u16,2,1,f); u16=2; fwrite(&u16,2,1,f);
  u32=rate; fwrite(&u32,4,1,f); u32=rate*4; fwrite(&u32,4,1,f);
  u16=4; fwrite(&u16,2,1,f); u16=16; fwrite(&u16,2,1,f);
  fwrite("data",1,4,f); fwrite(&dataLen,4,1,f);
  for (unsigned int i=0; i<n; i++) { fwrite(&l[i],2,1,f); fwrite(&r[i],2,1,f); }
  fclose(f);
}

int main(int argc, char** argv) {
  bool noteOn=!(argc>1 && strcmp(argv[1],"off")==0);
  const char* wavPath=argc>2?argv[2]:"ps1test.wav";
  const int rate=44100;

  DivEngine e;
  DivSample* s=new DivSample;
  s->depth=DIV_SAMPLE_DEPTH_16BIT;
  s->centerRate=rate;
  s->init(rate); // one second
  for (unsigned int i=0; i<s->samples; i++) {
    s->data16[i]=(short)round(16000.0*sin(2.0*M_PI*1000.0*i/rate));
  }
  e.song.sample.push_back(s);
  e.song.sampleLen=1;
  DivInstrument* ins=new DivInstrument;
  ins->type=DIV_INS_AMIGA;
  ins->amiga.initSample=0;
  e.song.ins.push_back(ins);
  e.song.insLen=1;

  DivPlatformPS1 ps1;
  DivConfig flags;
  ps1.init(&e,24,rate,flags);
  ps1.renderSamples(0);
  printf("arm=%s sampleMemUsage=%u loaded=%d\n",noteOn?"on":"off",(unsigned)ps1.getSampleMemUsage(0),ps1.isSampleLoaded(0,0));

  ps1.dispatch(DivCommand(DIV_CMD_INSTRUMENT,0,0));
  if (noteOn) ps1.dispatch(DivCommand(DIV_CMD_NOTE_ON,0,108)); // C-4: the sample plays at its center rate

  const size_t total=rate/2;
  const size_t tickLen=rate/60;
  std::vector<short> outL(total), outR(total);
  size_t pos=0;
  bool first=true;
  while (pos<total) {
    ps1.tick(true);
    if (first) {
      printf("chan0 freq(pitch reg)=%d (expect 4096 for C-4 on a 44100 Hz sample)\n",ps1.getChanState(0)->freq);
      first=false;
    }
    size_t n=std::min(tickLen,total-pos);
    short* bufs[2]={&outL[pos],&outR[pos]};
    ps1.acquire(bufs,n);
    pos+=n;
  }
  ps1.quit();
  writeWav(wavPath,outL,outR,rate);

  // measurements on the left channel (right is identical: same volume both sides)
  double sumSq=0; int peak=0;
  for (size_t i=0; i<total; i++) {
    sumSq+=(double)outL[i]*outL[i];
    if (abs(outL[i])>peak) peak=abs(outL[i]);
  }
  double rms=sqrt(sumSq/total);
  // zero crossings (rising) after the first 1024 samples
  size_t start=1024, crossings=0, firstX=0, lastX=0;
  for (size_t i=start+1; i<total; i++) {
    if (outL[i-1]<0 && outL[i]>=0) {
      if (crossings==0) firstX=i;
      lastX=i;
      crossings++;
    }
  }
  double zcHz=(crossings>1)?(double)(crossings-1)*rate/(double)(lastX-firstX):0.0;
  // Goertzel scan 20..5000 Hz, 1 Hz steps, for the dominant frequency
  double bestHz=0, bestP=0;
  for (int hz=20; hz<=5000; hz++) {
    double w=2.0*M_PI*hz/rate, c=2.0*cos(w), s1=0, s2=0;
    for (size_t i=start; i<total; i++) { double s0=outL[i]+c*s1-s2; s2=s1; s1=s0; }
    double p=s1*s1+s2*s2-c*s1*s2;
    if (p>bestP) { bestP=p; bestHz=hz; }
  }
  bool lrSame=memcmp(outL.data(),outR.data(),total*sizeof(short))==0;
  printf("frames=%u RMS=%.1f peak=%d zeroCrossHz=%.2f goertzelPeakHz=%.0f L==R:%d wav=%s\n",
    (unsigned)total,rms,peak,zcHz,bestP>0?bestHz:0.0,lrSame,wavPath);
  return 0;
}
