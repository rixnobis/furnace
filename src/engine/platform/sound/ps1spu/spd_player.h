// Ours. Host port of nugget's spdplayer (spdplayer/spdplayer.c: parsePackets, loadInternal,
// SPD_Poll, applyRegWrites, SPUInit/SPUResetVoice/SPUUpload) driving a PS1SPUBridge from an
// out.spudump/out.spubank pair. Used by the SPUDUMP exporter (to choose its loop encoding) and
// by the ps1spu_spdplay harness.
#ifndef _PS1SPU_SPD_PLAYER_H
#define _PS1SPU_SPD_PLAYER_H

#include <math.h>
#include <string.h>
#include <map>
#include <vector>
#include "../../../../ta-utils.h"
#include "bridge.h"

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
    // passing through more orders than the song has without a wait is a loop that never
    // waits: the player stops rather than spin
    unsigned int ordersWalked=0;
    while (true) {
      if ((size_t)stream>=fileSize/4) { stream=-1; return; } // (the player would run off)
      unsigned int header=word(fileBase,stream);
      unsigned int type=header>>24, len=header&0xffffff;
      size_t payload=stream+1;
      switch (type) {
        case 0x00: {
          unsigned int w=applyRegWrites(payload,len);
          // the poll that read a wait is the first of its ticks; a wait of 0 is a no-op
          if (w>0) {
            waitRemaining=w-1;
            stream=payload+len;
            return;
          }
          break;
        }
        case 0x01:
          if (len>=1 && word(fileBase,payload)>0) {
            waitRemaining=word(fileBase,payload)-1;
            stream=payload+len;
            return;
          }
          break;
        case 0x02:
          if (++ordersWalked>OrderCount) { stream=-1; return; }
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

// replay: one poll per tick; tick spacing from the stream's tick rate (as the engine computes
// it from a divider), or, if engineTicks is given, the engine's own spacing. the first
// skipTicks ticks are rendered but not kept; tickFrame, if given, gets the frame in rep each
// kept tick starts at.
inline void spdReplay(SPDPlayer& pl, PS1SPUBridge& spu, size_t ticks, const std::vector<int>* engineTicks, std::vector<short>& rep, std::vector<size_t>* tickFrame=NULL, size_t skipTicks=0) {
  std::vector<short> l, r;
  TickClock clk;
  for (size_t t=0; t<ticks; t++) {
    if (tickFrame && t>=skipTicks) tickFrame->push_back(rep.size()/2);
    pl.tickNow=(unsigned int)t;
    pl.Poll();
    int n=engineTicks?(*engineTicks)[t]:clk.next(PS1SPUBridge::RATE,(double)pl.tickRateFP/65536.0);
    if (n<=0) continue;
    l.resize(n); r.resize(n);
    spu.render(l.data(),r.data(),n);
    if (t<skipTicks) continue;
    for (int i=0; i<n; i++) {
      rep.push_back(l[i]);
      rep.push_back(r[i]);
    }
  }
}

#endif
