// Ours. Headless positive/negative control for DivPlatformPS1.
// Instantiates the platform directly with a bare DivEngine (never init()ed) as parent,
// holding one 44100 Hz 1 kHz sine sample and one Amiga-type instrument.
// usage: ps1spu_test <on|off|two|time|fmt|adsr|pan> <out.wav>
//   on/off: one instance, note on / no note, 0.5 s
//   two:    instances A (note on) and B (no note) rendered alternately, tick by tick, 0.5 s
//   time:   one instance, note on, 10 s, prints the wall time of the render loop
//   fmt:    DIV_INS_PS1 instrument serialize/deserialize round trip (+ negative control)
//   adsr:   fast vs slow attack, then release after note off
//   pan:    full left vs full right
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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

// measurements on the left channel (right is identical: same volume both sides)
static void report(const char* tag, const std::vector<short>& outL, const std::vector<short>& outR, int rate, const char* wavPath) {
  const size_t total=outL.size();
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
  printf("%sframes=%u RMS=%.1f peak=%d zeroCrossHz=%.2f goertzelPeakHz=%.0f L==R:%d wav=%s\n",
    tag,(unsigned)total,rms,peak,zcHz,bestP>0?bestHz:0.0,lrSame,wavPath);
}

static void hexdump(const char* tag, const unsigned char* b, size_t off, size_t len) {
  printf("%s @%u:",tag,(unsigned)off);
  for (size_t i=0; i<len; i++) printf(" %02x",b[off+i]);
  printf("\n");
}

static bool samePS1(const DivInstrument& a, const DivInstrument& b) {
  const DivInstrumentPS1& x=a.ps1;
  const DivInstrumentPS1& y=b.ps1;
  bool ok=true;
#define CHK(f) if (x.f!=y.f) { printf("  MISMATCH %s: %d != %d\n",#f,(int)x.f,(int)y.f); ok=false; }
  CHK(ar) CHK(arExp) CHK(dr) CHK(sl) CHK(sr) CHK(srDec) CHK(srExp) CHK(rr) CHK(rrExp)
  CHK(reverb) CHK(noise) CHK(pitchMod)
#undef CHK
  if (a.type!=b.type) { printf("  MISMATCH type: %d != %d\n",(int)a.type,(int)b.type); ok=false; }
  if (a.name!=b.name) { printf("  MISMATCH name\n"); ok=false; }
  return ok;
}

static int fmtArm() {
  DivInstrument a;
  a.type=DIV_INS_PS1;
  a.name="psx";
  a.ps1.ar=60; a.ps1.arExp=true; a.ps1.dr=9; a.ps1.sl=5;
  a.ps1.sr=77; a.ps1.srDec=true; a.ps1.srExp=true; a.ps1.rr=21; a.ps1.rrExp=true;
  a.ps1.reverb=true; a.ps1.noise=false; a.ps1.pitchMod=true;
  printf("in-memory DIV_INS_PS1=%d ADSR1=%04x ADSR2=%04x\n",(int)DIV_INS_PS1,a.ps1.getADSR1(),a.ps1.getADSR2());

  SafeWriter w;
  w.init();
  a.putInsData2(&w,false);
  unsigned char* buf=w.getFinalBuf();
  size_t len=w.size();
  printf("serialized INS2 block: %u bytes\n",(unsigned)len);
  // INS2 | len(4) | version(2) | type(2) | features...
  hexdump("type field",buf,10,2);
  unsigned short fileType=buf[10]|(buf[11]<<8);
  bool typeOK=fileType==DIV_INS_FILE_PS1;
  printf("on-disk type=0x%04x (expect 0x7e00): %s\n",fileType,typeOK?"OK":"FAIL");
  size_t psOff=0, pos=12;
  while (pos+4<=len) {
    if (buf[pos]=='E' && buf[pos+1]=='N') break;
    unsigned short fl=buf[pos+2]|(buf[pos+3]<<8);
    if (buf[pos]=='p' && buf[pos+1]=='s') { psOff=pos; hexdump("\"ps\" feature",buf,pos,4+fl); }
    pos+=4+fl;
  }
  if (psOff==0) { printf("FAIL: no ps feature\n"); return 1; }

  DivInstrument b;
  {
    SafeReader r(buf,len);
    b.readInsData(r,DIV_ENGINE_VERSION,NULL);
  }
  bool rtOK=samePS1(a,b);
  printf("round trip: %s\n",rtOK?"all fields equal":"FAIL");

  // negative control: flip a bit in the ADSR1 payload (sustain level).
  std::vector<unsigned char> bad(buf,buf+len);
  bad[psOff+5]^=0x01;
  printf("negative control: flip byte @%u %02x->%02x\n",(unsigned)(psOff+5),buf[psOff+5],bad[psOff+5]);
  DivInstrument c;
  {
    SafeReader r(bad.data(),len);
    c.readInsData(r,DIV_ENGINE_VERSION,NULL);
  }
  bool caught=!samePS1(a,c);
  printf("negative control: %s\n",caught?"mismatch caught":"FAIL (not caught)");

  // future version: payload v2 with extra trailing bytes must still read v1 fields
  std::vector<unsigned char> fut;
  fut.insert(fut.end(),buf,buf+psOff+2);
  unsigned short fl=buf[psOff+2]|(buf[psOff+3]<<8);
  unsigned short nfl=fl+3;
  fut.push_back(nfl&0xff); fut.push_back(nfl>>8);
  fut.insert(fut.end(),buf+psOff+4,buf+psOff+4+fl);
  fut[psOff+4]=2; // version 2
  fut.push_back(0xaa); fut.push_back(0xbb); fut.push_back(0xcc);
  fut.insert(fut.end(),buf+psOff+4+fl,buf+len);
  unsigned int blk=fut.size()-8; // INS2 block length field
  memcpy(&fut[4],&blk,4);
  DivInstrument d;
  {
    SafeReader r(fut.data(),fut.size());
    d.readInsData(r,DIV_ENGINE_VERSION,NULL);
  }
  bool futOK=samePS1(a,d);
  printf("future ps v2 (+3 unknown bytes): %s\n",futOK?"v1 fields equal, rest skipped":"FAIL");

  // helper mapping
  bool mapOK=insTypeFromFile(DIV_INS_FILE_PS1)==DIV_INS_PS1 &&
    insTypeToFile(DIV_INS_AMIGA)==DIV_INS_AMIGA && insTypeFromFile(DIV_INS_KLATTSCH)==DIV_INS_KLATTSCH &&
    insTypeFromFile((unsigned short)DIV_INS_PS1)!=DIV_INS_PS1 &&
    insTypeToFile(insTypeFromFile((unsigned short)DIV_INS_PS1))==(unsigned short)DIV_INS_PS1;
  printf("type helpers (upstream pass-through, foreign %d kept distinct and written back): %s\n",(int)DIV_INS_PS1,mapOK?"OK":"FAIL");
  w.finish();
  return (typeOK && rtOK && caught && futOK && mapOK)?0:1;
}

static double rmsOf(const std::vector<short>& v, size_t from, size_t n) {
  double s=0;
  if (from>=v.size()) return 0;
  if (from+n>v.size()) n=v.size()-from;
  for (size_t i=from; i<from+n; i++) s+=(double)v[i]*v[i];
  return n?sqrt(s/n):0;
}

// render total samples tick by tick; at sample offOffset (rounded up to a tick) send note off.
// returns the sample index at which the note off tick happened.
static size_t renderNote(DivEngine& e, DivPlatformPS1& ps1, int panL, int panR, size_t total, size_t offAt, std::vector<short>& L, std::vector<short>& R) {
  const size_t tickLen=44100/60;
  L.assign(total,0); R.assign(total,0);
  ps1.init(&e,24,44100,DivConfig());
  ps1.renderSamples(0);
  ps1.dispatch(DivCommand(DIV_CMD_INSTRUMENT,0,0));
  if (panL>=0) ps1.dispatch(DivCommand(DIV_CMD_PANNING,0,panL,panR));
  ps1.dispatch(DivCommand(DIV_CMD_NOTE_ON,0,108));
  size_t pos=0, offPos=0;
  bool offSent=false;
  while (pos<total) {
    if (!offSent && pos>=offAt) {
      ps1.dispatch(DivCommand(DIV_CMD_NOTE_OFF,0));
      offSent=true;
      offPos=pos;
    }
    ps1.tick(true);
    size_t n=std::min(tickLen,total-pos);
    short* bufs[2]={&L[pos],&R[pos]};
    ps1.acquire(bufs,n);
    pos+=n;
  }
  ps1.quit();
  return offPos;
}

int main(int argc, char** argv) {
  const char* mode=argc>1?argv[1]:"on";
  if (strcmp(mode,"fmt")==0) return fmtArm();
  bool noteOn=strcmp(mode,"off")!=0;
  bool two=strcmp(mode,"two")==0;
  bool timed=strcmp(mode,"time")==0;
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

  if (strcmp(mode,"adsr")==0) {
    const size_t win=rate/20; // 50 ms
    std::vector<short> L, R;
    ins->type=DIV_INS_PS1;
    ins->ps1.rr=10;
    double rmsFast, rmsSlow;
    {
      DivPlatformPS1 p; ins->ps1.ar=0;
      renderNote(e,p,-1,-1,rate/2,(size_t)-1,L,R);
      rmsFast=rmsOf(L,0,win);
      printf("attack ar=0  (ADSR1=%04x): RMS first 50 ms=%.1f, 400-450 ms=%.1f\n",ins->ps1.getADSR1(),rmsFast,rmsOf(L,rate*4/10,win));
    }
    {
      DivPlatformPS1 p; ins->ps1.ar=60;
      renderNote(e,p,-1,-1,rate/2,(size_t)-1,L,R);
      rmsSlow=rmsOf(L,0,win);
      printf("attack ar=60 (ADSR1=%04x): RMS first 50 ms=%.1f, 400-450 ms=%.1f  slow/fast=%.4f\n",ins->ps1.getADSR1(),rmsSlow,rmsOf(L,rate*4/10,win),rmsSlow/rmsFast);
    }
    {
      DivPlatformPS1 p; ins->ps1.ar=0;
      size_t offPos=renderNote(e,p,-1,-1,rate/2,rate/5,L,R);
      // linear release, shift rr<=11: level drops by 8<<(11-rr) per sample from 0x7fff
      double expMs=1000.0*(32767.0/(8<<(11-ins->ps1.rr)))/rate;
      printf("release rr=%d linear (ADSR2=%04x): note off at sample %u; expected ~%.1f ms to 0\n",ins->ps1.rr,ins->ps1.getADSR2(),(unsigned)offPos,expMs);
      printf("  RMS 10 ms before off=%.1f\n",rmsOf(L,offPos-rate/100,rate/100));
      size_t silentAt=0;
      for (size_t i=offPos; i<L.size(); i++) {
        bool allZero=true;
        for (size_t j=i; j<L.size(); j++) if (L[j]!=0) { allZero=false; break; }
        if (allZero) { silentAt=i; break; }
      }
      for (int k=0; k<8; k++) printf("  +%2d..%2d ms RMS=%.1f\n",k*10,k*10+10,rmsOf(L,offPos+k*rate/100,rate/100));
      printf("  output exactly 0 from %.1f ms after note off to end\n",1000.0*(silentAt-offPos)/rate);
    }
    return 0;
  }
  if (strcmp(mode,"pan")==0) {
    std::vector<short> L, R;
    {
      DivPlatformPS1 p;
      renderNote(e,p,255,0,rate/4,(size_t)-1,L,R);
      printf("pan full left  (L=255 R=0): RMS(L)=%.1f RMS(R)=%.1f\n",rmsOf(L,0,L.size()),rmsOf(R,0,R.size()));
    }
    {
      DivPlatformPS1 p;
      renderNote(e,p,0,255,rate/4,(size_t)-1,L,R);
      printf("pan full right (L=0 R=255): RMS(L)=%.1f RMS(R)=%.1f\n",rmsOf(L,0,L.size()),rmsOf(R,0,R.size()));
    }
    {
      DivPlatformPS1 p;
      renderNote(e,p,-1,-1,rate/4,(size_t)-1,L,R);
      printf("pan default (center): RMS(L)=%.1f RMS(R)=%.1f\n",rmsOf(L,0,L.size()),rmsOf(R,0,R.size()));
    }
    return 0;
  }

  DivPlatformPS1 ps1, ps1b;
  DivConfig flags;
  ps1.init(&e,24,rate,flags);
  ps1.renderSamples(0);
  if (two) {
    ps1b.init(&e,24,rate,flags);
    ps1b.renderSamples(0);
    ps1b.dispatch(DivCommand(DIV_CMD_INSTRUMENT,0,0));
  }
  printf("arm=%s sampleMemUsage=%u loaded=%d\n",mode,(unsigned)ps1.getSampleMemUsage(0),ps1.isSampleLoaded(0,0));

  ps1.dispatch(DivCommand(DIV_CMD_INSTRUMENT,0,0));
  if (noteOn) ps1.dispatch(DivCommand(DIV_CMD_NOTE_ON,0,108)); // C-4: the sample plays at its center rate

  const size_t total=timed?(size_t)rate*10:(size_t)rate/2;
  const size_t tickLen=rate/60;
  std::vector<short> outL(total), outR(total), bL(two?total:0), bR(two?total:0);
  size_t pos=0;
  bool first=true;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC,&t0);
  while (pos<total) {
    ps1.tick(true);
    if (two) ps1b.tick(true);
    if (first) {
      printf("chan0 freq(pitch reg)=%d (expect 4096 for C-4 on a 44100 Hz sample)\n",ps1.getChanState(0)->freq);
      first=false;
    }
    size_t n=std::min(tickLen,total-pos);
    short* bufs[2]={&outL[pos],&outR[pos]};
    ps1.acquire(bufs,n);
    if (two) {
      short* bufsB[2]={&bL[pos],&bR[pos]};
      ps1b.acquire(bufsB,n);
    }
    pos+=n;
  }
  clock_gettime(CLOCK_MONOTONIC,&t1);
  ps1.quit();
  if (two) ps1b.quit();
  writeWav(wavPath,outL,outR,rate);

  if (timed) {
    double ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)/1e6;
    printf("rendered %.1f s of audio in %.1f ms wall\n",(double)total/rate,ms);
  }
  report(two?"A: ":"",outL,outR,rate,wavPath);
  if (two) report("B: ",bL,bR,rate,"-");
  return 0;
}
