// Ours. SPUDUMP self-consistency harness for the PS1 SPUDUMP ROM export.
// A host port of nugget's spdplayer (spdplayer/spdplayer.c: parsePackets, loadInternal,
// SPD_Poll, applyRegWrites, SPUInit/SPUResetVoice/SPUUpload) drives a fresh PS1SPUBridge
// (the vendored pcsx-redux core) from the exported out.spudump/out.spubank, bypassing
// Furnace's engine; its output is compared against Furnace driving the same core.
// usage:
//   [SPD_PASSES=2] ps1spu_spdplay check <song.fur> <song.spudump> <bank.spubank> [furnace-cli.wav] [master]
//     SPD_PASSES=2 also compares the loop-back pass (default 1)
//     replay (SPD_LoadEx(song,bank), one poll per tick at the engine's tick spacing) vs the
//     engine reference (playSub, nextTick, the platform's acquire for each tick's samples,
//     i.e. the raw core output before Furnace's mixer), then vs the CLI render if given.
//     master: main volume the player is set to (SPD_SetMasterVolume), default 0x3fff (the
//     value the bridge programs); the player's own default is 0x3800.
//     also: packet counts, and the three checks of the old exporter's bugs (sample start
//     addresses vs the bank base, ADSR packing, bit 15 of voice volumes).
//   ps1spu_spdplay bit15
//     platform-level: voice volume writes for pan/volume extremes never set bit 15.
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <map>
#include <set>
#include <vector>
#include "../../../engine.h"
#include "../../ps1.h"

void reportError(String what) {
  logE("%s",what);
}

static bool readFile(const char* path, std::vector<unsigned char>& out) {
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

// samples (interleaved stereo) between ticks: DivEngine::nextTick's cycles/clockDrift rule.
struct TickClock {
  double drift=0;
  int next(double rate, double divider) {
    int cycles=(int)(rate/divider);
    drift+=fmod(rate,divider);
    if (drift>=divider) {
      drift-=divider;
      cycles++;
    }
    return cycles;
  }
};

// ---- host port of spdplayer.c ----
struct SPDWriteLog {
  unsigned int tick;
  unsigned short off, val;
};

struct SPDPlayer {
  // spdplayer.c limits
  static const unsigned MAX_ORDERS=256, MAX_PATTERNS=256, MAX_SAMPLES=128, MAX_MACROS=256, MAX_MACRO_WRITES=8;
  PS1SPUBridge* spu;
  std::vector<SPDWriteLog> log;
  unsigned int tickNow=0;
  unsigned outOfRange=0;
  std::map<int,unsigned> pktCount; // by type, every packet parsePackets walks (song file)
  std::map<int,unsigned> bankPktCount;

  unsigned VoiceCount=24, OrderCount=0, CurrentOrder=0, PatternCount=0, SampleCount=0;
  const unsigned char* fileBase=NULL;
  size_t fileSize=0;
  long stream=-1; // word index into the file, -1 = NULL
  unsigned int waitRemaining=0;
  unsigned int masterVolume=0x3800;
  unsigned short orderTable[MAX_ORDERS];
  unsigned int orderFlags=0, orderLoopTarget=0;
  unsigned int patternOffsets[MAX_PATTERNS];
  struct { unsigned short spuAddr, length, loopAddr, flags; } samples[MAX_SAMPLES];
  struct { unsigned short off[MAX_MACRO_WRITES], val[MAX_MACRO_WRITES]; unsigned char n; } macros[MAX_MACROS];
  unsigned macroCount=0;
  unsigned int tickRateFP=50<<16;
  size_t loadWrites=0;
  bool wrapped=false; // advanceOrder looped back
  bool stopped=false;
  // bank uploads: (byte address, length incl. DMA rounding)
  std::vector<std::pair<unsigned,unsigned>> uploads;

  unsigned int word(const unsigned char* base, size_t idx) {
    const unsigned char* p=base+idx*4;
    return p[0]|(p[1]<<8)|(p[2]<<16)|((unsigned int)p[3]<<24);
  }
  void reg(unsigned int off, unsigned short val) {
    // spuBase[offset/2] = value, spuBase = 1F801C00h
    off&=~1U;
    if (off>=0x200) {
      outOfRange++;
      return;
    }
    log.push_back({tickNow,(unsigned short)off,val});
    spu->writeReg(0xc00+off,val);
  }
  void SPUInit() {
    reg(0x180,masterVolume);
    reg(0x182,masterVolume);
    reg(0x1aa,0);
    reg(0x188,0); reg(0x18a,0);
    reg(0x18c,0xffff); reg(0x18e,0xffff);
    reg(0x1ac,4);
    reg(0x1b0,0); reg(0x1b2,0);
    reg(0x190,0); reg(0x192,0);
    reg(0x194,0); reg(0x196,0);
    reg(0x198,0); reg(0x19a,0);
    reg(0x1b4,0); reg(0x1b6,0);
    reg(0x1aa,0x8000);
  }
  void SPUResetVoice(int v) {
    unsigned int b=v*16;
    reg(b+0x0,0); reg(b+0x2,0); reg(b+0x4,0); reg(b+0x6,0);
    reg(b+0x8,0x000f); reg(b+0xc,0); reg(b+0xe,0); reg(b+0xa,0);
  }
  // DMA in 64-byte blocks: ceil(size/64) blocks from data (bytes past the buffer read as 0),
  // SPU transfer address wrapping at the end of RAM.
  void SPUUpload(unsigned int spuAddr, const unsigned char* buf, size_t bufLen, size_t at, unsigned int size) {
    unsigned int blocks=size>>6;
    if (size&0x3f) blocks++;
    unsigned int total=blocks*64;
    std::vector<unsigned char> data(total,0);
    for (unsigned int i=0; i<total; i++) if (at+i<bufLen) data[i]=buf[at+i];
    uploads.push_back({spuAddr,total});
    unsigned int a=spuAddr&(PS1SPUBridge::RAM_SIZE-1);
    unsigned int done=0;
    while (done<total) {
      unsigned int n=MIN(total-done,PS1SPUBridge::RAM_SIZE-a);
      spu->writeRAM(a,data.data()+done,n);
      done+=n;
      a=0;
    }
  }
  void parsePackets(const unsigned char* base, size_t size, bool skipSamples, std::map<int,unsigned>& counts) {
    size_t ptr=4, end=size/4;
    while (ptr<end) {
      unsigned int header=word(base,ptr);
      unsigned int type=header>>24, len=header&0xffffff;
      size_t payload=ptr+1;
      counts[type]++;
      switch (type) {
        case 0x10:
          if (len>=2) {
            orderFlags=word(base,payload);
            orderLoopTarget=word(base,payload+1);
            OrderCount=len-2;
            if (OrderCount>MAX_ORDERS) OrderCount=MAX_ORDERS;
            for (unsigned i=0; i<OrderCount; i++) orderTable[i]=word(base,payload+2+i)&0xffff;
          }
          break;
        case 0x11:
          if (len>=1 && PatternCount<MAX_PATTERNS) patternOffsets[PatternCount++]=word(base,payload);
          break;
        case 0x13:
          if (len>=2) {
            unsigned idx=word(base,payload)&0xffff;
            if (idx<MAX_MACROS) {
              unsigned n=MIN(len-1,MAX_MACRO_WRITES);
              macros[idx].n=n;
              for (unsigned j=0; j<n; j++) {
                unsigned int w=word(base,payload+1+j);
                macros[idx].off[j]=w>>16;
                macros[idx].val[j]=w&0xffff;
              }
              if (idx>=macroCount) macroCount=idx+1;
            }
          }
          break;
        case 0x20:
          if (len>=1) {
            SampleCount=MIN(word(base,payload),MAX_SAMPLES);
            for (unsigned i=0; i<SampleCount; i++) {
              unsigned int w0=word(base,payload+1+i*2), w1=word(base,payload+2+i*2);
              samples[i].spuAddr=w0>>16; samples[i].length=w0&0xffff;
              samples[i].loopAddr=w1>>16; samples[i].flags=w1&0xffff;
            }
          }
          break;
        case 0x21:
          if (!skipSamples && len>=1) {
            SPUUpload(word(base,payload)*8,base,size,(payload+1)*4,(len-1)*4);
          }
          break;
        case 0x30:
          if (len>=1) tickRateFP=word(base,payload);
          break;
        case 0x45:
          if (len>=1) VoiceCount=MIN(word(base,payload),24U);
          break;
        default:
          break;
      }
      ptr=payload+len;
    }
  }
  unsigned LoadEx(const unsigned char* data, size_t size, const unsigned char* sampleData, size_t sampleSize) {
    static const char magic[16]={'P','S','X','S','P','U','D','U','M','P','v','1','r','1',0,0};
    if (size<16 || memcmp(data,magic,16)!=0) return 0;
    SPUInit();
    fileBase=data; fileSize=size;
    if (sampleData && sampleSize>16) {
      if (memcmp(sampleData,magic,16)==0) parsePackets(sampleData,sampleSize,false,bankPktCount);
      parsePackets(data,size,true,pktCount);
    } else {
      parsePackets(data,size,false,pktCount);
    }
    for (int i=0; i<24; i++) SPUResetVoice(i);
    reg(0x1aa,0xc000); // SPUUnMute
    loadWrites=log.size();
    if (OrderCount>0 && PatternCount>0) {
      CurrentOrder=0;
      if (orderTable[0]<PatternCount) stream=patternOffsets[orderTable[0]]/4;
    }
    return VoiceCount;
  }
  void applyMacro(unsigned idx, unsigned voice) {
    if (idx>=macroCount) return;
    for (unsigned i=0; i<macros[idx].n; i++) reg((unsigned short)(macros[idx].off[i]+voice*0x10),macros[idx].val[i]);
  }
  unsigned int applyRegWrites(size_t payload, unsigned int count) {
    unsigned int waitAccum=0;
    for (unsigned int i=0; i<count; i++) {
      unsigned int w=word(fileBase,payload+i);
      unsigned short off=w>>16, val=w&0xffff;
      if (off==0xefff) waitAccum+=val;
      else if (off>=0xf000) applyMacro(off&0x0fff,val);
      else reg(off,val);
    }
    return waitAccum;
  }
  void advanceOrder() {
    CurrentOrder++;
    if (CurrentOrder>=OrderCount) {
      wrapped=true;
      if (orderFlags&1) {
        CurrentOrder=orderLoopTarget;
      } else {
        CurrentOrder=OrderCount-1;
        stream=-1;
        return;
      }
    }
    unsigned short pat=orderTable[CurrentOrder];
    stream=(pat<PatternCount)?(long)(patternOffsets[pat]/4):-1;
  }
  void Poll() {
    if (waitRemaining>0) {
      waitRemaining--;
      return;
    }
    if (stream<0) return;
    while (true) {
      if ((size_t)stream>=fileSize/4) { stream=-1; return; } // (the player would run off)
      unsigned int header=word(fileBase,stream);
      unsigned int type=header>>24, len=header&0xffffff;
      size_t payload=stream+1;
      switch (type) {
        case 0x00: {
          unsigned int w=applyRegWrites(payload,len);
          if (w>0) {
            waitRemaining=w;
            stream=payload+len;
            return;
          }
          break;
        }
        case 0x01:
          if (len>=1) waitRemaining=word(fileBase,payload);
          stream=payload+len;
          return;
        case 0x02:
          stream=payload+len;
          advanceOrder();
          if (stream<0) return;
          continue;
        case 0x30:
          if (len>=1) tickRateFP=word(fileBase,payload);
          break;
        default:
          break;
      }
      stream=payload+len;
    }
  }
};

struct Cmp {
  size_t n=0, diffs=0, first=(size_t)-1;
  double rmsRef=0, rmsDiff=0;
  int maxDiff=0;
};

static Cmp compare(const std::vector<short>& a, const std::vector<short>& b) {
  Cmp c;
  c.n=MIN(a.size(),b.size());
  double sr=0, sd=0;
  for (size_t i=0; i<c.n; i++) {
    int d=(int)a[i]-(int)b[i];
    if (d!=0) {
      c.diffs++;
      if (c.first==(size_t)-1) c.first=i;
    }
    if (abs(d)>c.maxDiff) c.maxDiff=abs(d);
    sr+=(double)b[i]*b[i];
    sd+=(double)d*d;
  }
  if (c.n) {
    c.rmsRef=sqrt(sr/c.n);
    c.rmsDiff=sqrt(sd/c.n);
  }
  return c;
}

static void printCmp(const char* tag, const Cmp& c, double samplesPerTick) {
  printf("  %s: %zu frames compared, ",tag,c.n/2);
  if (c.diffs==0) {
    printf("BYTE-IDENTICAL\n");
    return;
  }
  double db=(c.rmsDiff>0 && c.rmsRef>0)?20*log10(c.rmsDiff/c.rmsRef):0;
  printf("%zu differing samples, diff RMS %.2f (ref RMS %.2f, %.1f dB), max %d, first at frame %zu (%.4f s, tick %.1f)\n",
    c.diffs,c.rmsDiff,c.rmsRef,db,c.maxDiff,c.first/2,(c.first/2)/44100.0,(c.first/2)/samplesPerTick);
}

static bool readWav(const char* path, std::vector<short>& out, int& rate) {
  std::vector<unsigned char> f;
  if (!readFile(path,f) || f.size()<44) return false;
  size_t p=12;
  int ch=0, bits=0;
  rate=0;
  while (p+8<=f.size()) {
    unsigned int len=f[p+4]|(f[p+5]<<8)|(f[p+6]<<16)|((unsigned)f[p+7]<<24);
    if (memcmp(&f[p],"fmt ",4)==0) {
      ch=f[p+10]|(f[p+11]<<8);
      rate=f[p+12]|(f[p+13]<<8)|(f[p+14]<<16)|(f[p+15]<<24);
      bits=f[p+22]|(f[p+23]<<8);
    } else if (memcmp(&f[p],"data",4)==0) {
      if (ch!=2 || bits!=16) return false;
      size_t n=MIN((size_t)len,f.size()-p-8)/2;
      out.resize(n);
      memcpy(out.data(),&f[p+8],n*2);
      return true;
    }
    p+=8+len+(len&1);
  }
  return false;
}

// psx-spx "SPU Voice Volume/ADSR": 1F801C08h bits 15 attack mode, 14-10 attack shift, 9-8
// attack step, 7-4 decay shift, 3-0 sustain level; 1F801C0Ah bit 15 sustain mode, 14 sustain
// direction (1=decrease), 12-8 sustain shift, 7-6 sustain step, 5 release mode, 4-0 release shift.
static unsigned short packADSR1(const DivInstrumentPS1& p) {
  unsigned int attackShift=(p.ar>>2)&31, attackStep=p.ar&3;
  return (p.arExp?(1<<15):0)|(attackShift<<10)|(attackStep<<8)|((p.dr&15)<<4)|(p.sl&15);
}
static unsigned short packADSR2(const DivInstrumentPS1& p) {
  unsigned int susShift=(p.sr>>2)&31, susStep=p.sr&3;
  return (p.srExp?(1<<15):0)|(p.srDec?(1<<14):0)|(susShift<<8)|(susStep<<6)|(p.rrExp?(1<<5):0)|(p.rr&31);
}

class DivPS1SPDHarness {
  public:
  // the engine driving the core, one tick at a time: raw core output and the tick spacing
  static double divider;
  // passes: how many times through the song (2 = the loop-back pass as well)
  static bool reference(DivEngine& e, std::vector<short>& out, std::vector<int>& tickLen, int passes=1) {
    DivDispatch* d=e.disCont[0].dispatch;
    e.got.rate=PS1SPUBridge::RATE;
    e.stop();
    e.repeatPattern=false;
    e.curOrder=0;
    e.freelance=false;
    e.playing=false;
    e.extValuePresent=false;
    e.remainingLoops=-1;
    e.playSub(false);
    divider=e.divider;
    std::vector<short> l, r;
    int ends=0;
    while (true) {
      if (e.nextTick() && ++ends>=passes) break;
      if (!e.playing) break;
      int n=e.cycles;
      tickLen.push_back(n);
      if (n<=0) continue;
      l.resize(n); r.resize(n);
      short* bufs[2]={l.data(),r.data()};
      d->acquire(bufs,n);
      for (int i=0; i<n; i++) {
        out.push_back(l[i]);
        out.push_back(r[i]);
      }
    }
    e.playing=false;
    return true;
  }
};

double DivPS1SPDHarness::divider=0;

// replay: one poll per tick; tick spacing from the stream's tick rate (as the engine computes
// it from a divider), or, if engineTicks is given, the engine's own spacing.
static void replay(SPDPlayer& pl, PS1SPUBridge& spu, size_t ticks, const std::vector<int>* engineTicks, std::vector<short>& rep) {
  std::vector<short> l, r;
  TickClock clk;
  for (size_t t=0; t<ticks; t++) {
    pl.tickNow=(unsigned int)t;
    pl.Poll();
    int n=engineTicks?(*engineTicks)[t]:clk.next(PS1SPUBridge::RATE,(double)pl.tickRateFP/65536.0);
    if (n<=0) continue;
    l.resize(n); r.resize(n);
    spu.render(l.data(),r.data(),n);
    for (int i=0; i<n; i++) {
      rep.push_back(l[i]);
      rep.push_back(r[i]);
    }
  }
}

// best lag (|lag|<=64 frames, left channel, first 10 s) and least-squares gain of a against b,
// then the residual of b-g*a over the overlap.
static void alignedCmp(const char* tag, const std::vector<short>& a, const std::vector<short>& b) {
  size_t fa=a.size()/2, fb=b.size()/2;
  size_t win=MIN((size_t)441000,MIN(fa,fb));
  int bestLag=0;
  double best=-1e300;
  for (int lag=-64; lag<=64; lag++) {
    double acc=0;
    for (size_t i=64; i+64<win; i++) acc+=(double)a[i*2]*b[(i+lag)*2];
    if (acc>best) { best=acc; bestLag=lag; }
  }
  double saa=0, sab=0, sbb=0;
  size_t n=0;
  for (size_t i=64; i+64<MIN(fa,fb); i++) {
    for (int c=0; c<2; c++) {
      double x=a[i*2+c], y=b[(i+bestLag)*2+c];
      saa+=x*x; sab+=x*y; sbb+=y*y;
    }
    n+=2;
  }
  double g=saa>0?sab/saa:0;
  double res=0;
  int maxRes=0;
  for (size_t i=64; i+64<MIN(fa,fb); i++) {
    for (int c=0; c<2; c++) {
      double d=b[(i+bestLag)*2+c]-g*a[i*2+c];
      res+=d*d;
      if (fabs(d)>maxRes) maxRes=(int)fabs(d);
    }
  }
  double rmsB=sqrt(sbb/MAX((size_t)1,n)), rmsR=sqrt(res/MAX((size_t)1,n));
  printf("  %s, aligned: lag %+d frames, gain %.4f (%.2f dB), residual RMS %.2f (%.1f dB re CLI RMS %.2f), max %d\n",
    tag,bestLag,g,g>0?20*log10(g):0.0,rmsR,(rmsR>0&&rmsB>0)?20*log10(rmsR/rmsB):-999.0,rmsB,maxRes);
}

static int checkArm(const char* furPath, const char* spdPath, const char* bankPath, const char* cliWav, unsigned int master) {
  std::vector<unsigned char> fur, spd, bank;
  if (!readFile(furPath,fur) || !readFile(spdPath,spd) || !readFile(bankPath,bank)) {
    printf("cannot read inputs\n");
    return 1;
  }
  DivEngine e;
  e.preInit(true);
  e.setAudio(DIV_AUDIO_DUMMY);
  if (!e.init()) return 1;
  unsigned char* b=new unsigned char[fur.size()];
  memcpy(b,fur.data(),fur.size());
  if (!e.load(b,fur.size(),furPath)) { printf("load failed\n"); return 1; }
  DivPlatformPS1* ps1=(DivPlatformPS1*)e.getDispatch(0);
  // snapshot of what the platform uploaded, before the reference run
  const unsigned char* mem=(const unsigned char*)ps1->getSampleMem(0);
  std::vector<unsigned char> sampleMem(mem,mem+PS1SPUBridge::RAM_SIZE);
  size_t memLen=ps1->getSampleMemUsage(0);
  unsigned int sampleBase=ps1->getSampleBase();
  std::set<unsigned int> sampleStarts;
  for (const DivMemoryEntry& m: ps1->getMemCompo(0)->entries) {
    if (m.type==DIV_MEMORY_SAMPLE) sampleStarts.insert((unsigned int)m.begin);
  }

  std::vector<short> ref;
  std::vector<int> tickLen;
  const char* passEnv=getenv("SPD_PASSES");
  int passes=passEnv?atoi(passEnv):1;
  if (passes<1) passes=1;
  printf("  passes: %d\n",passes);
  DivPS1SPDHarness::reference(e,ref,tickLen,passes);

  // replay
  PS1SPUBridge spu;
  SPDPlayer pl;
  pl.spu=&spu;
  pl.masterVolume=master;
  if (!pl.LoadEx(spd.data(),spd.size(),bank.data(),bank.size())) { printf("SPD_LoadEx failed\n"); return 1; }
  std::vector<short> rep;
  size_t ticks=tickLen.size();
  replay(pl,spu,ticks,NULL,rep);
  double spt=PS1SPUBridge::RATE/((double)pl.tickRateFP/65536.0);

  printf("%s\n",furPath);
  printf("  files: song %zu bytes, bank %zu bytes\n",spd.size(),bank.size());
  auto pc=[](std::map<int,unsigned>& m) {
    unsigned tot=0;
    String s;
    for (auto& i: m) {
      tot+=i.second;
      s+=fmt::sprintf(" %02x:%u",i.first,i.second);
    }
    return fmt::sprintf("%u packets (type:count%s)",tot,s);
  };
  printf("  song: %s\n",pc(pl.pktCount).c_str());
  printf("  bank: %s\n",pc(pl.bankPktCount).c_str());
  printf("  player: %u orders, %u patterns, %u samples, voice count %u, tick rate %.4f Hz, loop flag %u target %u\n",
    pl.OrderCount,pl.PatternCount,pl.SampleCount,pl.VoiceCount,pl.tickRateFP/65536.0,pl.orderFlags&1,pl.orderLoopTarget);
  printf("  ticks: %zu (engine), stream %s after them; out-of-range writes %u\n",ticks,
    pl.wrapped?"wrapped (order end reached)":(pl.stream<0?"stopped":"still running"),pl.outOfRange);
  int tickMismatch=0;
  {
    TickClock c2;
    for (size_t t=0; t<ticks; t++) if (c2.next(PS1SPUBridge::RATE,(double)pl.tickRateFP/65536.0)!=tickLen[t]) tickMismatch++;
  }
  printf("  tick spacing: %d of %zu ticks differ from the engine's (engine divider %.9f Hz, stream %u/65536 = %.9f Hz)\n",
    tickMismatch,ticks,DivPS1SPDHarness::divider,pl.tickRateFP,pl.tickRateFP/65536.0);
  printf("  lengths: replay %zu frames, reference %zu frames\n",rep.size()/2,ref.size()/2);
  printCmp("replay vs engine reference (raw core)",compare(rep,ref),spt);
  if (tickMismatch) {
    // the same stream with the engine's tick spacing: isolates the 16.16 tick rate rounding
    PS1SPUBridge spu2;
    SPDPlayer pl2;
    pl2.spu=&spu2;
    pl2.masterVolume=master;
    pl2.LoadEx(spd.data(),spd.size(),bank.data(),bank.size());
    std::vector<short> rep2;
    replay(pl2,spu2,ticks,&tickLen,rep2);
    printCmp("replay at the engine's tick spacing vs engine reference",compare(rep2,ref),spt);
  }

  if (cliWav) {
    std::vector<short> cli;
    int rate=0;
    if (readWav(cliWav,cli,rate)) {
      printf("  CLI wav: %zu frames at %d Hz\n",cli.size()/2,rate);
      printCmp("replay vs CLI render",compare(rep,cli),spt);
      alignedCmp("replay vs CLI render",rep,cli);
      alignedCmp("engine reference vs CLI render",ref,cli);
    } else {
      printf("  CLI wav: unreadable or not s16 stereo\n");
    }
  }

  // known bug 1: sample start addresses carry the bank base
  {
    bool bankOk=true;
    unsigned int dataBase=0;
    if (!pl.uploads.empty()) {
      dataBase=pl.uploads[0].first;
      // the bank's data packet at the sample base holds the platform's SPU RAM image
      size_t p=4;
      while (p<bank.size()/4) {
        unsigned int h=pl.word(bank.data(),p);
        if ((h>>24)==0x21) {
          unsigned int a=pl.word(bank.data(),p+1)*8;
          if (a==sampleBase) bankOk=(memcmp(&bank[(p+2)*4],&sampleMem[sampleBase],memLen-sampleBase)==0);
          break;
        }
        p+=1+(h&0xffffff);
      }
    }
    unsigned starts=0, good=0, bad=0;
    for (SPDWriteLog& w: pl.log) {
      if (w.off<0x180 && (w.off&15)==6 && w.val!=0) {
        starts++;
        unsigned int a=(unsigned int)w.val*8;
        if (sampleStarts.count(a) && a>=dataBase && a<memLen) good++;
        else {
          if (bad<3) printf("    bad start write: voice %d value %04x (byte %05x) at tick %u\n",w.off>>4,w.val,a,w.tick);
          bad++;
        }
      }
    }
    printf("  check start addresses: bank data base %05x (platform sample base %05x), image %s; %u start writes, %u at a sample's first block, %u not: %s\n",
      dataBase,sampleBase,bankOk?"byte-identical":"DIFFERS",starts,good,bad,(bankOk && bad==0 && dataBase==sampleBase)?"PASS":"FAIL");
  }
  // known bug 2: ADSR packing (independent psx-spx packing of each PS1 instrument)
  {
    std::set<std::pair<unsigned short,unsigned short>> expect;
    DivInstrumentPS1 def;
    expect.insert({packADSR1(def),packADSR2(def)});
    for (int i=0; i<e.song.insLen; i++) {
      if (e.song.ins[i]->type==DIV_INS_PS1) expect.insert({packADSR1(e.song.ins[i]->ps1),packADSR2(e.song.ins[i]->ps1)});
    }
    unsigned short a1[24], a2[24];
    bool s1[24]={}, s2[24]={};
    unsigned kons=0, good=0, bad=0;
    std::set<std::pair<unsigned short,unsigned short>> seen;
    // only the stream's writes (not SPUInit/SPUResetVoice/unmute)
    for (size_t i=pl.loadWrites; i<pl.log.size(); i++) {
      SPDWriteLog& w=pl.log[i];
      if (w.off<0x180 && (w.off&15)==8) { a1[w.off>>4]=w.val; s1[w.off>>4]=true; }
      if (w.off<0x180 && (w.off&15)==0xa) { a2[w.off>>4]=w.val; s2[w.off>>4]=true; }
      if (w.off==0x188 || w.off==0x18a) {
        for (int bit=0; bit<16; bit++) {
          if (!(w.val&(1<<bit))) continue;
          int v=bit+(w.off==0x18a?16:0);
          kons++;
          if (s1[v] && s2[v] && expect.count({a1[v],a2[v]})) {
            good++;
            seen.insert({a1[v],a2[v]});
          } else {
            if (bad<3) printf("    key-on voice %d with ADSR %04x/%04x at tick %u matches no instrument\n",v,s1[v]?a1[v]:0,s2[v]?a2[v]:0,w.tick);
            bad++;
          }
        }
      }
    }
    String pairs;
    for (auto& p: seen) pairs+=fmt::sprintf(" %04x/%04x",p.first,p.second);
    printf("  check ADSR: %u key-ons, %u with an instrument's psx-spx packing, %u not; distinct pairs:%s: %s\n",
      kons,good,bad,pairs.c_str(),(bad==0 && kons>0)?"PASS":"FAIL");
  }
  // known bug 3: bit 15 (sweep mode) of voice volumes
  {
    unsigned vols=0, bit15=0, panned=0;
    for (SPDWriteLog& w: pl.log) {
      if (w.off<0x180 && ((w.off&15)==0 || (w.off&15)==2)) {
        vols++;
        if (w.val&0x8000) bit15++;
      }
    }
    // volume pairs with L!=R in the stream (panned notes)
    unsigned short vl[24]={};
    for (SPDWriteLog& w: pl.log) {
      if (w.off<0x180 && (w.off&15)==0) vl[w.off>>4]=w.val;
      if (w.off<0x180 && (w.off&15)==2 && vl[w.off>>4]!=w.val) panned++;
    }
    printf("  check volume bit 15: %u voice volume writes (%u panned L!=R), %u with bit 15: %s\n",vols,panned,bit15,bit15==0?"PASS":"FAIL");
  }
  fflush(stdout);
  _exit(0);
}

// platform-level bit 15 check: pan and volume extremes, register dump on.
static int bit15Arm() {
  DivEngine e;
  DivPlatformPS1 p;
  p.init(&e,24,44100,DivConfig());
  p.toggleRegisterDump(true);
  p.getRegisterWrites().clear();
  struct { int l, r, vol; } cases[]={{255,255,127},{0,255,127},{255,0,127},{0,0,127},{255,255,0},{0,255,0},{128,128,1},{255,255,-1}};
  unsigned writes=0, bit15=0;
  for (auto& c: cases) {
    p.dispatch(DivCommand(DIV_CMD_VOLUME,0,c.vol));
    p.dispatch(DivCommand(DIV_CMD_PANNING,0,c.l,c.r));
    p.tick();
    for (DivRegWrite& w: p.getRegisterWrites()) {
      if (w.addr==0x0 || w.addr==0x2) {
        writes++;
        if (w.val&0x8000) bit15++;
        printf("  vol %d pan %d/%d: reg %x = %04x\n",c.vol,c.l,c.r,w.addr,w.val);
      }
    }
    p.getRegisterWrites().clear();
  }
  printf("bit15: %u voice volume writes, %u with bit 15: %s\n",writes,bit15,bit15==0?"PASS":"FAIL");
  return bit15==0?0:1;
}

int main(int argc, char** argv) {
  const char* mode=argc>1?argv[1]:"";
  if (strcmp(mode,"check")==0 && argc>=5) {
    unsigned int master=argc>6?(unsigned int)strtoul(argv[6],NULL,0):0x3fff;
    return checkArm(argv[2],argv[3],argv[4],argc>5?argv[5]:NULL,master);
  }
  if (strcmp(mode,"bit15")==0) return bit15Arm();
  printf("usage: ps1spu_spdplay check <song.fur> <song.spudump> <bank.spubank> [cli.wav] [master] | bit15\n");
  return 1;
}
