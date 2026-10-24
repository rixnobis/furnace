// Ours. Headless positive/negative control for DivPlatformPS1.
// Instantiates the platform directly with a bare DivEngine (never init()ed) as parent,
// holding one 44100 Hz 1 kHz sine sample and one Amiga-type instrument.
// usage: ps1spu_test <on|off|two|time|fmt|adsr|pan|reverb|noise|pmod|rvbregs|spu|upload> <out.wav>
//   on/off: one instance, note on / no note, 0.5 s
//   two:    instances A (note on) and B (no note) rendered alternately, tick by tick, 0.5 s
//   time:   one instance, note on, 10 s, prints the wall time of the render loop
//   fmt:    DIV_INS_PS1 instrument serialize/deserialize round trip (+ negative control)
//   adsr:   fast vs slow attack, then release after note off
//   pan:    full left vs full right
//   reverb: 10 ms burst with reverb off/presets: tail RMS after the dry end, tail length
//   noise:  NON on/off: spectral flatness/centroid, centroid vs noise clock (1Dxx)
//   pmod:   voice 0 5 Hz sine modulating voice 1 1 kHz (PMON): frequency range
//   rvbregs <song.fur> [out.fur]: reverb register flags: derived work area per preset,
//           .fur round trip of custom registers, overflow warnings
//   spu [tmpdir] [other.vag]: SPU ADPCM sample depth: on-disk id 0xE1 and foreign guard,
//           conversion, loop flags, VAG round trip, PS1 loop hint, upload as stored
//   upload <song.fur> <bank.vab>: uploaded SPU RAM bytes of each sample vs the VAB bodies
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
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

// ---- reverb / noise / pitch modulation arms ----

struct NoteOn {
  int ch, ins, note;
};

// fresh instance with the given system flags; optional 1Dxx (noise clock) before the notes;
// all notes keyed on the first tick; total samples rendered tick by tick.
static void renderSeq(DivEngine& e, const DivConfig& flags, const std::vector<NoteOn>& ons, int noiseFreq, size_t total, std::vector<short>& L, std::vector<short>& R) {
  const size_t tickLen=44100/60;
  L.assign(total,0); R.assign(total,0);
  DivPlatformPS1 p;
  p.init(&e,24,44100,flags);
  p.renderSamples(0);
  if (noiseFreq>=0) p.dispatch(DivCommand(DIV_CMD_STD_NOISE_FREQ,0,noiseFreq));
  for (const NoteOn& n: ons) {
    p.dispatch(DivCommand(DIV_CMD_INSTRUMENT,n.ch,n.ins));
    p.dispatch(DivCommand(DIV_CMD_NOTE_ON,n.ch,n.note));
  }
  size_t pos=0;
  while (pos<total) {
    p.tick(true);
    size_t n=std::min(tickLen,total-pos);
    short* bufs[2]={&L[pos],&R[pos]};
    p.acquire(bufs,n);
    pos+=n;
  }
  p.quit();
}

static int addSample(DivEngine& e, DivSample* s) {
  e.song.sample.push_back(s);
  e.song.sampleLen=e.song.sample.size();
  return e.song.sampleLen-1;
}

static int addPS1Ins(DivEngine& e, int sample, bool reverb, bool noise, bool pitchMod) {
  DivInstrument* ins=new DivInstrument;
  ins->type=DIV_INS_PS1;
  ins->amiga.initSample=sample;
  ins->ps1.reverb=reverb;
  ins->ps1.noise=noise;
  ins->ps1.pitchMod=pitchMod;
  e.song.ins.push_back(ins);
  e.song.insLen=e.song.ins.size();
  return e.song.insLen-1;
}

static DivSample* newSample(int len, int rate) {
  DivSample* s=new DivSample;
  s->depth=DIV_SAMPLE_DEPTH_16BIT;
  s->centerRate=rate;
  s->init(len);
  return s;
}

static size_t lastNonZero(const std::vector<short>& L, const std::vector<short>& R) {
  size_t last=0;
  for (size_t i=0; i<L.size(); i++) if (L[i]!=0 || R[i]!=0) last=i;
  return last;
}

static int reverbArm(DivEngine& e) {
  const int rate=44100;
  // 10 ms decaying noise burst, one-shot: the dry note is over after 10 ms.
  DivSample* b=newSample(441,rate);
  unsigned int lcg=1;
  for (int i=0; i<441; i++) {
    lcg=lcg*1103515245+12345;
    b->data16[i]=(short)((((int)((lcg>>16)&0x7fff))-16384)*(441-i)/441);
  }
  int sb=addSample(e,b);
  int insRev=addPS1Ins(e,sb,true,false,false);
  int insDry=addPS1Ins(e,sb,false,false,false);
  const size_t total=rate*8;
  std::vector<short> L, R;
  bool ok=true;

  // dry end: the last nonzero sample with reverb off.
  DivConfig off;
  renderSeq(e,off,{{0,insRev,108}},-1,total,L,R);
  size_t dryEnd=lastNonZero(L,R);
  double offTail=rmsOf(L,dryEnd+rate/5,rate*3/10)+rmsOf(R,dryEnd+rate/5,rate*3/10);
  printf("reverb off (preset 0, ins reverb flag on): dry ends at %.1f ms; RMS(L)+RMS(R) 200-500 ms after = %.3f\n",1000.0*dryEnd/rate,offTail);
  if (offTail!=0) ok=false;

  for (int preset: {1,5,7,2}) {
    DivConfig f;
    f.set("reverbPreset",preset);
    renderSeq(e,f,{{0,insRev,108}},-1,total,L,R);
    double w[3];
    for (int k=0; k<3; k++) w[k]=rmsOf(L,dryEnd+rate/5+k*rate/10,rate/10);
    double tail=rmsOf(L,dryEnd+rate/5,rate*3/10);
    // tail length: first 50 ms window (after the dry end) whose RMS is under 1/1000 (-60 dB)
    // of the loudest 50 ms window, and the last nonzero output sample.
    double peakW=0;
    size_t peakPos=dryEnd;
    for (size_t pos=dryEnd; pos+rate/20<=total; pos+=rate/20) {
      double r=rmsOf(L,pos,rate/20);
      if (r>peakW) { peakW=r; peakPos=pos; }
    }
    size_t t60=0;
    for (size_t pos=peakPos; pos+rate/20<=total; pos+=rate/20) {
      if (rmsOf(L,pos,rate/20)<peakW/1000) { t60=pos; break; }
    }
    size_t last=lastNonZero(L,R);
    printf("reverb %-13s (size %05x): tail RMS 200-500 ms=%.1f [200-300 %.1f, 300-400 %.1f, 400-500 %.1f]; -60 dB at %s%.0f ms; last nonzero %.0f ms%s\n",
      DivPlatformPS1::reverbPresetName(preset),DivPlatformPS1::reverbSize(preset),tail,w[0],w[1],w[2],
      t60?"":">",1000.0*((t60?t60:total)-dryEnd)/rate,1000.0*(last-dryEnd)/rate,(last>=total-1)?" (still sounding at end)":"");
    if (!(tail>0 && w[0]>w[1] && w[1]>w[2])) ok=false;
  }
  // negative control: preset on, instrument reverb flag off.
  {
    DivConfig f;
    f.set("reverbPreset",5);
    renderSeq(e,f,{{0,insDry,108}},-1,total,L,R);
    double t=rmsOf(L,dryEnd+rate/5,rate*3/10)+rmsOf(R,dryEnd+rate/5,rate*3/10);
    printf("reverb Hall, ins reverb flag off: RMS 200-500 ms after = %.3f\n",t);
    if (t!=0) ok=false;
  }
  printf("reverb arm: %s\n",ok?"OK":"FAIL");
  return ok?0:1;
}

// in-place radix-2 FFT
static void fft(std::vector<double>& re, std::vector<double>& im) {
  const size_t n=re.size();
  for (size_t i=1, j=0; i<n; i++) {
    size_t bit=n>>1;
    for (; j&bit; bit>>=1) j^=bit;
    j^=bit;
    if (i<j) { std::swap(re[i],re[j]); std::swap(im[i],im[j]); }
  }
  for (size_t len=2; len<=n; len<<=1) {
    double a=-2.0*M_PI/len;
    for (size_t i=0; i<n; i+=len) {
      for (size_t k=0; k<len/2; k++) {
        double wr=cos(a*k), wi=sin(a*k);
        double xr=re[i+k+len/2]*wr-im[i+k+len/2]*wi;
        double xi=re[i+k+len/2]*wi+im[i+k+len/2]*wr;
        re[i+k+len/2]=re[i+k]-xr; im[i+k+len/2]=im[i+k]-xi;
        re[i+k]+=xr; im[i+k]+=xi;
      }
    }
  }
}

// averaged Hann-windowed power spectrum (4096 bins) of [from,from+frames*4096)
static void spectrum(const std::vector<short>& x, size_t from, int frames, double& flatness, double& centroid, double& peakHz, double& peakShare) {
  const size_t n=4096;
  std::vector<double> P(n/2,0.0);
  for (int f=0; f<frames; f++) {
    std::vector<double> re(n), im(n,0.0);
    for (size_t i=0; i<n; i++) re[i]=x[from+f*n+i]*(0.5-0.5*cos(2.0*M_PI*i/(n-1)));
    fft(re,im);
    for (size_t k=0; k<n/2; k++) P[k]+=re[k]*re[k]+im[k]*im[k];
  }
  double logSum=0, sum=0, fSum=0, best=0;
  size_t bestK=1;
  for (size_t k=1; k<n/2; k++) {
    double p=P[k]+1e-9;
    logSum+=log(p); sum+=p; fSum+=p*k*44100.0/n;
    if (p>best) { best=p; bestK=k; }
  }
  const double m=n/2-1;
  flatness=exp(logSum/m)/(sum/m);
  centroid=fSum/sum;
  peakHz=bestK*44100.0/n;
  double near=0;
  for (size_t k=(bestK>3?bestK-3:1); k<=bestK+3 && k<n/2; k++) near+=P[k];
  peakShare=near/sum;
}

static int noiseArm(DivEngine& e) {
  const int rate=44100;
  DivSample* s=newSample(rate,rate);
  for (int i=0; i<rate; i++) s->data16[i]=(short)round(16000.0*sin(2.0*M_PI*1000.0*i/rate));
  int ss=addSample(e,s);
  int insNoise=addPS1Ins(e,ss,false,true,false);
  int insTone=addPS1Ins(e,ss,false,false,false);
  std::vector<short> L, R;
  double fl, ce, pk, sh;
  bool ok=true;
  renderSeq(e,DivConfig(),{{0,insTone,108}},-1,rate/2,L,R);
  spectrum(L,rate/10,4,fl,ce,pk,sh);
  printf("noise flag off: flatness=%.4f centroid=%.0f Hz peak=%.0f Hz (share of energy within +-3 bins: %.3f)\n",fl,ce,pk,sh);
  if (!(sh>0.9 && fabs(pk-1000)<20)) ok=false;
  double lastCe=0;
  for (int nf: {-1,0x08,0x18,0x28,0x38,0x3f}) {
    renderSeq(e,DivConfig(),{{0,insNoise,108}},nf,rate/2,L,R);
    spectrum(L,rate/10,4,fl,ce,pk,sh);
    printf("noise flag on, clock %s%02x: flatness=%.4f centroid=%.0f Hz peak share=%.3f RMS=%.1f\n",nf<0?"default ":"",nf<0?0x20:nf,fl,ce,sh,rmsOf(L,rate/10,rate/4));
    if (nf>=0) {
      if (ce<=lastCe) ok=false;
      lastCe=ce;
    }
    // broadband from the default (mid) clock up; the lowest clocks step the LFSR only a few
    // times in the window, so their energy sits near DC.
    if ((nf<0 || nf>=0x20) && sh>0.5) ok=false;
  }
  printf("noise arm: %s\n",ok?"OK":"FAIL");
  return ok?0:1;
}

// instantaneous frequency of the left channel from rising zero crossings (linearly
// interpolated), averaged over 10 ms windows; min/max over [from,to).
static void freqRange(const std::vector<short>& x, size_t from, size_t to, double& fMin, double& fMax) {
  const int rate=44100;
  std::vector<double> xs;
  for (size_t i=from+1; i<to; i++) {
    if (x[i-1]<0 && x[i]>=0) xs.push_back((i-1)+(double)(-x[i-1])/(double)(x[i]-x[i-1]));
  }
  fMin=1e9; fMax=0;
  const double win=rate/100.0;
  size_t j=0;
  for (double t=from; t+win<=to; t+=win) {
    while (j<xs.size() && xs[j]<t) j++;
    size_t k=j;
    while (k+1<xs.size() && xs[k+1]<t+win) k++;
    if (k<=j) continue;
    double f=(double)(k-j)*rate/(xs[k]-xs[j]);
    fMin=std::min(fMin,f); fMax=std::max(fMax,f);
  }
}

static int pmodArm(DivEngine& e) {
  const int rate=44100;
  DivSample* c=newSample(rate,rate);
  for (int i=0; i<rate; i++) c->data16[i]=(short)round(16000.0*sin(2.0*M_PI*1000.0*i/rate));
  int sc=addSample(e,c);
  // modulator: 5 Hz sine, one second looped (5 whole cycles)
  DivSample* m=newSample(rate,rate);
  for (int i=0; i<rate; i++) m->data16[i]=(short)round(16000.0*sin(2.0*M_PI*5.0*i/rate));
  m->loop=true; m->loopStart=0; m->loopEnd=rate; m->loopMode=DIV_SAMPLE_LOOP_FORWARD;
  int sm=addSample(e,m);
  int insMod=addPS1Ins(e,sm,false,false,false);
  int insCarPM=addPS1Ins(e,sc,false,false,true);
  int insCar=addPS1Ins(e,sc,false,false,false);
  std::vector<short> L, R;
  double lo, hi, lo0, hi0, loV0, hiV0;
  renderSeq(e,DivConfig(),{{0,insMod,108},{1,insCar,108}},-1,rate*3/4,L,R);
  freqRange(L,rate/10,rate*7/10,lo0,hi0);
  printf("pitch mod off (voice 0 5 Hz sine, voice 1 1 kHz): voice-1 frequency over 10 ms windows min=%.1f max=%.1f Hz\n",lo0,hi0);
  renderSeq(e,DivConfig(),{{0,insMod,108},{1,insCarPM,108}},-1,rate*3/4,L,R);
  freqRange(L,rate/10,rate*7/10,lo,hi);
  printf("pitch mod on  (voice 1 PMON): min=%.1f max=%.1f Hz\n",lo,hi);
  // voice 0 cannot be modulated: the flag on voice 0 alone must change nothing.
  renderSeq(e,DivConfig(),{{0,insCarPM,108}},-1,rate*3/4,L,R);
  freqRange(L,rate/10,rate*7/10,loV0,hiV0);
  printf("pitch mod flag on voice 0 (ignored): min=%.1f max=%.1f Hz\n",loV0,hiV0);
  // without modulation the spread is the estimator's own (+-1 crossing per 10 ms window).
  bool ok=(hi0-lo0)<20 && (hi-lo)>200 && (hiV0-loV0)<20;
  printf("pitch mod arm: %s\n",ok?"OK":"FAIL");
  return ok?0:1;
}

// ---- reverb register flags arm ----

static void setRegs(DivConfig& f, const unsigned short* regs) {
  for (int i=0; i<32; i++) f.set(String("rvb_")+DivPlatformPS1::reverbRegName(i),(int)regs[i]);
}

// tail RMS (L, 200-500 ms after the dry end) of the 10 ms burst through the given flags;
// optionally the instance's warnings and dropped-sample count after renderSamples.
static double burstTail(DivEngine& e, const DivConfig& f, int ins, size_t dryEnd, std::vector<short>& L, std::vector<short>& R, int* warn=NULL, int* dropped=NULL) {
  const int rate=44100;
  const size_t tickLen=rate/60, total=rate*3;
  L.assign(total,0); R.assign(total,0);
  DivPlatformPS1 p;
  p.init(&e,24,rate,f);
  p.renderSamples(0);
  if (warn) *warn=p.reverbWarnings();
  if (dropped) *dropped=p.reverbSamplesDropped();
  p.dispatch(DivCommand(DIV_CMD_INSTRUMENT,0,ins));
  p.dispatch(DivCommand(DIV_CMD_NOTE_ON,0,108));
  for (size_t pos=0; pos<total; pos+=tickLen) {
    p.tick(true);
    size_t n=std::min(tickLen,total-pos);
    short* bufs[2]={&L[pos],&R[pos]};
    p.acquire(bufs,n);
  }
  p.quit();
  return rmsOf(L,dryEnd+rate/5,rate*3/10);
}

// usage: ps1spu_test rvbregs <song.fur> <out-custom.fur>
static int rvbRegsArm(const char* songPath, const char* outPath) {
  bool ok=true;
  // 1. derived work area size per preset vs psx-spx's listed size
  printf("derived work area (bytes) vs psx-spx table:\n");
  for (int p=1; p<DivPlatformPS1::REVERB_PRESETS; p++) {
    unsigned short r[32];
    int maxReg=10;
    for (int i=0; i<32; i++) r[i]=DivPlatformPS1::reverbPresetReg(p,i);
    for (int i=10; i<30; i++) if (r[i]>r[maxReg]) maxReg=i;
    unsigned int need=DivPlatformPS1::reverbRequired(r), tab=DivPlatformPS1::reverbSize(p);
    printf("  %-13s derived %05X  table %05X  table-derived %+6d  (largest address %s=%04X -> %05X)\n",
      DivPlatformPS1::reverbPresetName(p),need,tab,(int)tab-(int)need,DivPlatformPS1::reverbRegName(maxReg),r[maxReg],r[maxReg]*8);
    if (need>tab) ok=false;
  }

  DivEngine e;
  e.preInit(true);
  e.setAudio(DIV_AUDIO_DUMMY);
  if (!e.init()) return 1;
  // burst sample/instrument for the tail measurements (separate song state is fine: the
  // platform instances below read e.song only for samples/instruments).
  auto loadFile=[&](const unsigned char* data, size_t len, const char* name) {
    unsigned char* b=new unsigned char[len];
    memcpy(b,data,len);
    return e.load(b,len,name);
  };
  FILE* f=fopen(songPath,"rb");
  if (!f) { printf("cannot open %s\n",songPath); return 1; }
  fseek(f,0,SEEK_END);
  size_t len=ftell(f);
  fseek(f,0,SEEK_SET);
  std::vector<unsigned char> file(len);
  if (fread(file.data(),1,len,f)!=len) return 1;
  fclose(f);
  if (!loadFile(file.data(),len,songPath)) { printf("load failed\n"); return 1; }
  printf("loaded %s: flags have rvb_ keys: %s, reverbPreset=%d\n",songPath,
    e.song.systemFlags[0].has("rvb_vIIR")?"yes":"no",e.song.systemFlags[0].getInt("reverbPreset",-1));

  // 2. round trip: Hall with vIIR and mLSAME changed, saved through saveFur and reloaded.
  unsigned short hall[32], cust[32];
  for (int i=0; i<32; i++) hall[i]=cust[i]=DivPlatformPS1::reverbPresetReg(5,i);
  cust[2]=0x3000;  // vIIR 6000h -> 3000h
  cust[10]=0x1000; // mLSAME 15BAh -> 1000h
  DivConfig custom=e.song.systemFlags[0];
  custom.set("reverbPreset",DivPlatformPS1::REVERB_CUSTOM);
  setRegs(custom,cust);
  custom.set("rvb_mBASE",(int)((PS1SPUBridge::RAM_SIZE-DivPlatformPS1::reverbRequired(cust))>>3));
  e.song.systemFlags[0]=custom;
  SafeWriter* w=e.saveFur();
  if (w==NULL) { printf("saveFur failed\n"); return 1; }
  std::vector<unsigned char> saved(w->getFinalBuf(),w->getFinalBuf()+w->size());
  w->finish();
  delete w;
  if (outPath) {
    FILE* o=fopen(outPath,"wb");
    if (o) { fwrite(saved.data(),1,saved.size(),o); fclose(o); }
  }
  if (!loadFile(saved.data(),saved.size(),"custom.fur")) { printf("reload failed\n"); return 1; }
  // a copy: createNew below resets e.song.
  const DivConfig back=e.song.systemFlags[0];
  int same=0;
  for (int i=0; i<32; i++) {
    String k=String("rvb_")+DivPlatformPS1::reverbRegName(i);
    if (back.has(k) && back.getInt(k,-1)==(int)cust[i]) same++;
  }
  bool baseSame=back.getInt("rvb_mBASE",-1)==custom.getInt("rvb_mBASE",-2);
  bool presetSame=back.getInt("reverbPreset",-1)==DivPlatformPS1::REVERB_CUSTOM;
  DivPlatformPS1::ReverbSetup rsC;
  DivPlatformPS1::reverbResolve(back,rsC);
  printf("round trip (%u-byte .fur): %d/32 register flags equal, mBASE %s (%04X), reverbPreset %s (%d); resolved base %05X needs %u bytes, warn %d\n",
    (unsigned)saved.size(),same,baseSame?"equal":"DIFFERS",back.getInt("rvb_mBASE",-1),presetSame?"equal":"DIFFERS",back.getInt("reverbPreset",-1),rsC.base,rsC.required,rsC.warn);
  if (same!=32 || !baseSame || !presetSame || rsC.warn) ok=false;

  // tail renders: Hall (preset only) vs the reloaded custom flags, same burst.
  e.createNew("id0=225\n","Sony PlayStation",false);
  const int rate=44100;
  DivSample* b=newSample(441,rate);
  unsigned int lcg=1;
  for (int i=0; i<441; i++) {
    lcg=lcg*1103515245+12345;
    b->data16[i]=(short)((((int)((lcg>>16)&0x7fff))-16384)*(441-i)/441);
  }
  int sb=addSample(e,b);
  int ins=addPS1Ins(e,sb,true,false,false);
  std::vector<short> L, R, L2, R2;
  DivConfig fOff;
  burstTail(e,fOff,ins,0,L,R);
  size_t dryEnd=lastNonZero(L,R);
  DivConfig fHall;
  fHall.set("reverbPreset",5);
  double tHall=burstTail(e,fHall,ins,dryEnd,L,R);
  double tCust=burstTail(e,back,ins,dryEnd,L2,R2);
  printf("tail RMS 200-500 ms: Hall %.2f, custom (vIIR 3000h, mLSAME 1000h, reloaded) %.2f\n",tHall,tCust);
  if (tHall==tCust || L==L2) ok=false;
  // Hall written out as explicit register flags + table base: must equal the preset-only render.
  DivConfig fHallX;
  fHallX.set("reverbPreset",5);
  setRegs(fHallX,hall);
  fHallX.set("rvb_mBASE",(int)((PS1SPUBridge::RAM_SIZE-DivPlatformPS1::reverbSize(5))>>3));
  burstTail(e,fHallX,ins,dryEnd,L2,R2);
  bool eqX=(L==L2 && R==R2);
  printf("Hall as explicit register flags + table base vs preset only: %s\n",eqX?"bit-identical":"DIFFERENT");
  if (!eqX) ok=false;
  // informational: Hall at the derived (smaller) base instead of the table one.
  fHallX.set("rvb_mBASE",(int)((PS1SPUBridge::RAM_SIZE-DivPlatformPS1::reverbRequired(hall))>>3));
  burstTail(e,fHallX,ins,dryEnd,L2,R2);
  printf("Hall at derived base %05X vs table base %05X: %s\n",PS1SPUBridge::RAM_SIZE-DivPlatformPS1::reverbRequired(hall),
    PS1SPUBridge::RAM_SIZE-DivPlatformPS1::reverbSize(5),(L==L2 && R==R2)?"bit-identical":"different");

  // 3. overflow: a 300000-frame sample (about 171 KB of ADPCM) plus mLSAME=F000h (needs
  // about 480 KB): the area cannot sit above the samples.
  DivSample* big=newSample(300000,rate);
  for (int i=0; i<300000; i++) big->data16[i]=(short)(8000*sin(2*M_PI*440.0*i/rate));
  addSample(e,big);
  unsigned short over[32];
  memcpy(over,hall,sizeof(over));
  over[10]=0xF000;
  struct { const char* what; int base; } cases[]={
    {"mLSAME=F000h, base derived",-1},
    {"Hall regs, mBASE=FFF0h (past the end)",-2},
    {"Hall regs, mBASE=0010h (inside capture buffers)",0x10},
    {"mLSAME=FFFFh (more than all of SPU RAM)",-3},
  };
  for (auto& c: cases) {
    DivConfig fo;
    fo.set("reverbPreset",DivPlatformPS1::REVERB_CUSTOM);
    if (c.base==-3) {
      unsigned short huge[32];
      memcpy(huge,hall,sizeof(huge));
      huge[10]=0xffff;
      setRegs(fo,huge);
    } else {
      setRegs(fo,(c.base==-1)?over:hall);
    }
    if (c.base==-2) fo.set("rvb_mBASE",0xfff0);
    else if (c.base>=0) fo.set("rvb_mBASE",c.base);
    DivPlatformPS1::ReverbSetup rs;
    DivPlatformPS1::reverbResolve(fo,rs);
    int warn=0, dropped=0;
    double t=burstTail(e,fo,ins,dryEnd,L2,R2,&warn,&dropped);
    printf("overflow %-48s: needs %06X, base %05X, warn %d%s%s%s, samples dropped %d, tail RMS %.2f\n",c.what,rs.required,rs.base,warn,
      (warn&DivPlatformPS1::REVERB_WARN_CLAMPED)?" CLAMPED":"",(warn&DivPlatformPS1::REVERB_WARN_LOW)?" LOW":"",(warn&DivPlatformPS1::REVERB_WARN_ALIAS)?" ALIAS":"",dropped,t);
    if (warn==0 && dropped==0) ok=false;
  }
  printf("rvbregs arm: %s\n",ok?"OK":"FAIL");
  fflush(stdout);
  _exit(ok?0:1);
}

// ---- SPU ADPCM sample depth ----

static bool fileBytes(const char* path, std::vector<unsigned char>& out) {
  FILE* f=fopen(path,"rb");
  if (!f) return false;
  fseek(f,0,SEEK_END);
  long len=ftell(f);
  fseek(f,0,SEEK_SET);
  out.resize(len);
  bool ok=fread(out.data(),1,len,f)==(size_t)len;
  fclose(f);
  return ok;
}

static bool sameSPU(DivSample* a, DivSample* b) {
  return a->depth==DIV_SAMPLE_DEPTH_SPU_ADPCM && b->depth==DIV_SAMPLE_DEPTH_SPU_ADPCM &&
    a->lengthSPU==b->lengthSPU && a->samples==b->samples && memcmp(a->dataSPU,b->dataSPU,a->lengthSPU)==0;
}

// spu [tmpdir] [other.vag]: depth id mapping, conversion, .fur sample block round trip with
// the foreign-depth guard, loop flag sync, VAG round trip (and an external VAG), the PS1 loop
// hint, and the platform uploading an SPU ADPCM sample as stored.
static int spuArm(const char* tmpDir, const char* extVag) {
  bool ok=true;
  auto check=[&](const char* what, bool c) {
    printf("  %-72s %s\n",what,c?"OK":"FAIL");
    if (!c) ok=false;
  };
  printf("depth ids: in-memory SPU_ADPCM=%d, on disk 0x%02X\n",(int)DIV_SAMPLE_DEPTH_SPU_ADPCM,sampleDepthToFile(DIV_SAMPLE_DEPTH_SPU_ADPCM));
  check("SPU_ADPCM <-> 0xE1",sampleDepthToFile(DIV_SAMPLE_DEPTH_SPU_ADPCM)==0xe1 && sampleDepthFromFile(0xe1)==DIV_SAMPLE_DEPTH_SPU_ADPCM);
  check("upstream depths pass through (0..16)",[]() {
    for (int i=0; i<=16; i++) if (sampleDepthToFile((DivSampleDepth)i)!=i || sampleDepthFromFile(i)!=(DivSampleDepth)i) return false;
    return true;
  }());
  check("raw 17 (future upstream) is not SPU ADPCM, written back as 17",
    sampleDepthFromFile(17)!=DIV_SAMPLE_DEPTH_SPU_ADPCM && sampleDepthToFile(sampleDepthFromFile(17))==17);

  // conversion from 16-bit: same bytes as the platform's own encoding
  const int len=1500, ls=280, le=1400;
  DivSample* s=newSample(len,44100);
  s->name="spu sine";
  for (int i=0; i<len; i++) s->data16[i]=(short)round(12000.0*sin(2.0*M_PI*441.0*i/44100.0));
  s->loop=true; s->loopStart=ls; s->loopEnd=le;
  std::vector<unsigned char> ref(PS1SPUBridge::encodedSize(len));
  size_t refLen=PS1SPUBridge::encodeADPCM(s->data16,len,ls,le,ref.data(),ref.size());
  s->convert(DIV_SAMPLE_DEPTH_SPU_ADPCM,(1U<<DIV_SAMPLE_DEPTH_16BIT)|(1U<<DIV_SAMPLE_DEPTH_SPU_ADPCM));
  printf("16-bit %d samples, loop %d..%d -> SPU ADPCM %u bytes, %u samples, loop %d..%d\n",len,ls,le,s->lengthSPU,s->samples,s->loopStart,s->loopEnd);
  check("converted bytes == PS1SPUBridge::encodeADPCM",s->lengthSPU==refLen && memcmp(s->dataSPU,ref.data(),refLen)==0);
  bool fl; int fs, fe;
  s->getSPULoop(fl,fs,fe);
  check("block flags describe loop 280..1400 (blocks 10..49)",fl && fs==280 && fe==1400);

  // .fur sample block (SMP2) round trip
  SafeWriter w;
  w.init();
  s->putSampleData(&w);
  std::vector<unsigned char> blk(w.getFinalBuf(),w.getFinalBuf()+w.size());
  w.finish();
  size_t depthOff=8+s->name.size()+1+12;
  printf("SMP2 block %u bytes, depth byte @%u = 0x%02X\n",(unsigned)blk.size(),(unsigned)depthOff,blk[depthOff]);
  check("on-disk depth byte is 0xE1",blk[depthOff]==0xe1);
  DivSample t;
  {
    SafeReader r(blk.data(),blk.size());
    t.readSampleData(r,DIV_ENGINE_VERSION);
  }
  check("read back: SPU ADPCM, bytes/loop identical",sameSPU(s,&t) && t.loop && t.loopStart==ls && t.loopEnd==le);
  std::vector<unsigned char> foreign=blk;
  foreign[depthOff]=17;
  DivSample u;
  {
    SafeReader r(foreign.data(),foreign.size());
    u.readSampleData(r,DIV_ENGINE_VERSION);
  }
  SafeWriter w2;
  w2.init();
  u.putSampleData(&w2);
  unsigned char back=w2.getFinalBuf()[depthOff];
  w2.finish();
  printf("raw depth 17 loads as in-memory %d, written back as %d\n",(int)u.depth,(int)back);
  check("raw 17 does not load as SPU ADPCM; written back as 17",u.depth!=DIV_SAMPLE_DEPTH_SPU_ADPCM && back==17);

  // loop flag sync: untouched when consistent, rewritten on a loop edit
  std::vector<unsigned char> before(s->dataSPU,s->dataSPU+s->lengthSPU);
  s->render((1U<<DIV_SAMPLE_DEPTH_16BIT)|(1U<<DIV_SAMPLE_DEPTH_SPU_ADPCM));
  check("render with consistent loop leaves the bytes untouched",memcmp(before.data(),s->dataSPU,s->lengthSPU)==0);
  s->loopStart=560;
  s->render((1U<<DIV_SAMPLE_DEPTH_16BIT)|(1U<<DIV_SAMPLE_DEPTH_SPU_ADPCM));
  s->getSPULoop(fl,fs,fe);
  int diff=0;
  for (unsigned int i=0; i<s->lengthSPU; i++) if (before[i]!=s->dataSPU[i]) diff++;
  printf("loop start 280 -> 560: %d bytes changed (flag bytes only: %s), flags now loop %d..%d\n",diff,
    [&]() { for (unsigned int i=0; i<s->lengthSPU; i++) if ((i&15)!=1 && before[i]!=s->dataSPU[i]) return "no"; return "yes"; }(),fs,fe);
  check("loop edit rewrites flags only, to 560..1400",fl && fs==560 && fe==1400 && diff>0);
  s->loop=false;
  s->render((1U<<DIV_SAMPLE_DEPTH_16BIT)|(1U<<DIV_SAMPLE_DEPTH_SPU_ADPCM));
  s->getSPULoop(fl,fs,fe);
  check("loop off: one-shot, end flag on the last block",!fl && fe==(int)s->samples);
  s->loop=true; s->loopStart=ls; s->loopEnd=le;
  s->render((1U<<DIV_SAMPLE_DEPTH_16BIT)|(1U<<DIV_SAMPLE_DEPTH_SPU_ADPCM));
  check("loop back to 280..1400 restores the original bytes",memcmp(before.data(),s->dataSPU,s->lengthSPU)==0);

  // VAG round trip
  String vagPath=String(tmpDir)+"/spu_roundtrip.vag";
  String vagPath2=String(tmpDir)+"/spu_roundtrip2.vag";
  check("saveVAG",s->saveVAG(vagPath.c_str()));
  std::vector<unsigned char> vag, vag2;
  fileBytes(vagPath.c_str(),vag);
  printf("VAG: %u bytes, header:",(unsigned)vag.size());
  for (int i=0; i<48; i++) printf("%s%02x",(i%16)?" ":"\n    ",vag[i]);
  printf("\n");
  DivSample v;
  String err;
  check("loadVAG",v.loadVAG(vag.data(),vag.size(),err));
  check("VAG import: bytes identical, rate, name, loop",sameSPU(s,&v) && v.centerRate==s->centerRate && v.name==s->name && v.loop && v.loopStart==ls && v.loopEnd==le);
  v.saveVAG(vagPath2.c_str());
  fileBytes(vagPath2.c_str(),vag2);
  check("export of the imported sample: file identical",vag==vag2);
  if (extVag!=NULL) {
    std::vector<unsigned char> ext;
    DivSample x;
    if (!fileBytes(extVag,ext) || !x.loadVAG(ext.data(),ext.size(),err)) {
      printf("external VAG %s: FAIL (%s)\n",extVag,err.c_str());
      ok=false;
    } else {
      x.getSPULoop(fl,fs,fe);
      printf("external VAG %s: %u data bytes, rate %d, name \"%s\", loop %s %d..%d\n",extVag,x.lengthSPU,x.centerRate,x.name.c_str(),x.loop?"on":"off",x.loopStart,x.loopEnd);
      check("external VAG data bytes kept as they are",x.lengthSPU<=ext.size()-48 && memcmp(x.dataSPU,ext.data()+48,x.lengthSPU)==0);
    }
  }

  // PS1 loop hint for a 16-bit sample, loop 30..100 (the sample editor's warning text)
  DivSample* h=newSample(1000,44100);
  h->loop=true; h->loopStart=30; h->loopEnd=100;
  printf("loop hint, 16-bit sample loop 30..100 on a PS1 song:\n");
  if (h->loopStart%28) printf("  \"%s\"\n",fmt::sprintf("PS1: loop start must be a multiple of 28 (try with %d)",h->spuAlignHint(h->loopStart)).c_str());
  if (h->loopEnd%28) printf("  \"%s\"\n",fmt::sprintf("PS1: loop end must be a multiple of 28 (try with %d)",h->spuAlignHint(h->loopEnd)).c_str());
  check("hint suggests 28 and 112",h->spuAlignHint(30)==28 && h->spuAlignHint(100)==112);
  delete h;

  // platform: SPU ADPCM sample uploaded as stored, next to a 16-bit one encoded as before
  DivEngine e;
  DivSample* p16=newSample(len,44100);
  for (int i=0; i<len; i++) p16->data16[i]=(short)round(12000.0*sin(2.0*M_PI*441.0*i/44100.0));
  p16->loop=true; p16->loopStart=ls; p16->loopEnd=le;
  // make the stored blocks deliberately differ from what encoding the decoded PCM would give:
  // the platform must not re-encode them.
  s->dataSPU[16*3+2]^=0x11;
  s->render((1U<<DIV_SAMPLE_DEPTH_16BIT)|(1U<<DIV_SAMPLE_DEPTH_SPU_ADPCM));
  addSample(e,p16);
  addSample(e,s);
  DivPlatformPS1 ps1;
  ps1.init(&e,24,44100,DivConfig());
  ps1.renderSamples(0);
  const DivMemoryComposition* mc=ps1.getMemCompo(0);
  const unsigned char* mem=(const unsigned char*)ps1.getSampleMem(0);
  bool up16=false, upSPU=false;
  for (const DivMemoryEntry& m: mc->entries) {
    if (m.type!=DIV_MEMORY_SAMPLE) continue;
    if (m.asset==0) up16=(m.end-m.begin==refLen && memcmp(mem+m.begin,ref.data(),refLen)==0);
    if (m.asset==1) upSPU=(m.end-m.begin==s->lengthSPU && memcmp(mem+m.begin,s->dataSPU,s->lengthSPU)==0);
  }
  ps1.quit();
  check("PS1 upload: 16-bit sample encoded as before",up16);
  check("PS1 upload: SPU ADPCM sample bytes as stored (modified block kept)",upSPU);
  printf("spu: %s\n",ok?"all OK":"FAIL");
  return ok?0:1;
}

// upload <song.fur> <bank.vab>: load the song, render its samples into SPU RAM, and compare
// every uploaded sample with the VAB's ADPCM bodies.
static int uploadArm(const char* songPath, const char* vabPath) {
  std::vector<unsigned char> vab, song;
  if (!fileBytes(vabPath,vab) || vab.size()<32 || memcmp(vab.data(),"pBAV",4)!=0) { printf("bad VAB %s\n",vabPath); return 1; }
  if (!fileBytes(songPath,song)) { printf("cannot open %s\n",songPath); return 1; }
  int nprog=vab[18]|(vab[19]<<8), nvag=vab[22]|(vab[23]<<8);
  size_t obase=32+128*16+(size_t)nprog*16*32;
  std::vector<std::vector<unsigned char>> bodies;
  size_t o=obase+512;
  for (int i=0; i<nvag; i++) {
    size_t sz=(size_t)(vab[obase+2*i]|(vab[obase+2*i+1]<<8))<<3;
    bodies.push_back(std::vector<unsigned char>(vab.begin()+o,vab.begin()+o+sz));
    o+=sz;
  }
  DivEngine e;
  e.preInit(true);
  e.setAudio(DIV_AUDIO_DUMMY);
  if (!e.init()) return 1;
  unsigned char* b=new unsigned char[song.size()];
  memcpy(b,song.data(),song.size());
  if (!e.load(b,song.size(),songPath)) { printf("load failed\n"); return 1; }
  DivDispatch* d=e.getDispatch(0);
  const DivMemoryComposition* mc=d->getMemCompo(0);
  const unsigned char* mem=(const unsigned char*)d->getSampleMem(0);
  int compared=0, equal=0, spu=0;
  for (const DivMemoryEntry& m: mc->entries) {
    if (m.type!=DIV_MEMORY_SAMPLE) continue;
    compared++;
    if (e.song.sample[m.asset]->depth==DIV_SAMPLE_DEPTH_SPU_ADPCM) spu++;
    int match=-1;
    for (int i=0; i<nvag && match<0; i++) {
      if (bodies[i].size()==m.end-m.begin && memcmp(bodies[i].data(),mem+m.begin,bodies[i].size())==0) match=i;
    }
    if (match>=0) equal++;
    else printf("  sample %d (%s): %u bytes in SPU RAM, no identical VAB body\n",m.asset,e.song.sample[m.asset]->name.c_str(),(unsigned)(m.end-m.begin));
  }
  printf("%s: %d VAB bodies; %d samples (%d SPU ADPCM) uploaded; %d compared, %d byte-identical to a VAB body: %s\n",
    songPath,nvag,e.song.sampleLen,spu,compared,equal,(compared>0 && equal==compared)?"ALL EQUAL":"MISMATCH");
  // like the rvbregs arm: an init()ed engine's quit() can hang without audio/MIDI devices.
  fflush(stdout);
  _exit((compared>0 && equal==compared)?0:1);
}

int main(int argc, char** argv) {
  const char* mode=argc>1?argv[1]:"on";
  if (strcmp(mode,"spu")==0) return spuArm(argc>2?argv[2]:"/tmp",argc>3?argv[3]:NULL);
  if (strcmp(mode,"upload")==0) {
    if (argc<4) { printf("usage: ps1spu_test upload <song.fur> <bank.vab>\n"); return 1; }
    return uploadArm(argv[2],argv[3]);
  }
  if (strcmp(mode,"fmt")==0) return fmtArm();
  if (strcmp(mode,"rvbregs")==0) return rvbRegsArm(argc>2?argv[2]:"papers/ps1-examples/keynote.fur",argc>3?argv[3]:NULL);
  if (strcmp(mode,"reverb")==0 || strcmp(mode,"noise")==0 || strcmp(mode,"pmod")==0) {
    DivEngine e;
    if (strcmp(mode,"reverb")==0) return reverbArm(e);
    if (strcmp(mode,"noise")==0) return noiseArm(e);
    return pmodArm(e);
  }
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
