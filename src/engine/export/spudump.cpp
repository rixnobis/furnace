/**
 * Furnace Tracker - multi-system chiptune tracker
 * Copyright (C) 2021-2026 tildearrow and contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */


// SPUDUMP export for the PS1 SPU (DivPlatformPS1).
// format: standards/SPUDUMP.md (PSXSPUDUMPv1r1). player: nugget spdplayer (spdplayer.c), whose
// behavior wins where the two differ:
// - a register write packet's inline waits (0xEFFF) are summed and applied after the whole
//   packet, so an inline wait is only ever placed at the end of a packet.
// - a wait of N (packet or inline) holds for N+1 polls: the poll that read it plus N more.
//   an inline wait of 0 does not stop the poll at all, so a 1-tick gap is a wait packet of 0.
// - the loop point packet (0x03) is ignored; looping is the order table's loop flag and target.
// layout: out.spudump is the stream (metadata, tick rate, voice count, order table, pattern
// headers, pattern streams); out.spubank holds the sample directory and the sample data
// (the SPU RAM image renderSamples built, at its base, plus the zeroed reverb work area), for
// SPD_LoadEx(song,bank).
// a "pattern" here is one visit of the song's order list in playback order (split at the loop
// point when it is not on a row 0), so the order table is 0..N-1 and the loop target is the
// pattern where the song loops. each pattern starts with a snapshot of the register state
// (voices touched so far and the global state), so it can be entered cold (SPD_Seek).

#include "spudump.h"
#include "../engine.h"
#include "../platform/ps1.h"
#include "../../ta-log.h"
#include <fmt/printf.h>

#define SPD_PKT_REG_WRITE 0x00
#define SPD_PKT_WAIT 0x01
#define SPD_PKT_END_PATTERN 0x02
#define SPD_PKT_ORDER_TABLE 0x10
#define SPD_PKT_PATTERN_HDR 0x11
#define SPD_PKT_SAMPLE_DIR 0x20
#define SPD_PKT_SAMPLE_DATA 0x21
#define SPD_PKT_TICK_RATE 0x30
#define SPD_PKT_TITLE 0x40
#define SPD_PKT_AUTHOR 0x41
#define SPD_PKT_COMMENT 0x43
#define SPD_PKT_VOICE_COUNT 0x45

#define SPD_VREG_WAIT 0xefff

// spdplayer limits (MAX_ORDERS, MAX_PATTERNS, MAX_SAMPLES)
#define SPD_MAX_PATTERNS 256
#define SPD_MAX_SAMPLES 128

// register offsets relative to 1F801C00h
#define SPD_KON_LO 0x188
#define SPD_KON_HI 0x18a
#define SPD_KOFF_LO 0x18c
#define SPD_KOFF_HI 0x18e

static void writeHeader(SafeWriter* w, unsigned char type, unsigned int len) {
  w->writeI((len&0xffffff)|((unsigned int)type<<24));
}

static void writeString(SafeWriter* w, unsigned char type, const String& str) {
  size_t len=str.size();
  size_t padded=(len+4)&~(size_t)3;
  writeHeader(w,type,padded/4);
  w->write(str.c_str(),len);
  for (size_t i=len; i<padded; i++) w->writeC(0);
}

static void writeWord(SafeWriter* w, unsigned char type, unsigned int val) {
  writeHeader(w,type,1);
  w->writeI(val);
}

static void writeMagic(SafeWriter* w) {
  w->write("PSXSPUDUMPv1r1\0\0",16);
}

// sample data packet: base (8-byte units) then data, padded with zeros to a whole number of
// 64-byte blocks (spdplayer DMAs ceil(size/64) blocks, reading past a shorter payload).
static void writeSampleData(SafeWriter* w, unsigned int addr, const unsigned char* data, size_t len) {
  size_t padded=(len+63)&~(size_t)63;
  writeHeader(w,SPD_PKT_SAMPLE_DATA,1+padded/4);
  w->writeI(addr>>3);
  if (data!=NULL) {
    w->write(data,len);
  } else {
    for (size_t i=0; i<len; i++) w->writeC(0);
  }
  for (size_t i=len; i<padded; i++) w->writeC(0);
}

// registers a pattern snapshot restores: voice volume, pitch, start, ADSR, repeat; main,
// reverb, CD and external volumes; PMON/NON/EON; mBASE; SPUCNT; the reverb block. never
// KON/KOFF, ENDX, the current ADSR volume, the transfer or IRQ registers.
static bool snapshotReg(unsigned int r) {
  if (r<0x180) {
    unsigned int v=r&15;
    return v!=0xc;
  }
  if (r>=0x180 && r<=0x186) return true;
  if (r>=0x190 && r<=0x19a) return true;
  if (r==0x1a2 || r==0x1aa) return true;
  if (r>=0x1b0 && r<=0x1b6) return true;
  if (r>=0x1c0 && r<0x200) return true;
  return false;
}

struct SPDTick {
  std::vector<unsigned int> words;
  unsigned int rate; // 16.16 Hz in effect after this tick
};

struct SPDPattern {
  int order;
  std::vector<unsigned int> snapshot;
  std::vector<SPDTick> ticks;
};

// one tick's batch followed by a gap of d ticks, spdplayer timing (see top of file).
static void emitBatch(std::vector<unsigned int>& out, std::vector<unsigned int>& words, unsigned int d, size_t& packets) {
  if (d==0) d=1;
  if (words.empty()) {
    out.push_back(((unsigned int)SPD_PKT_WAIT<<24)|1);
    out.push_back(d-1);
    packets++;
    return;
  }
  if (d>=2) {
    unsigned int w=d-1;
    while (w>0xffff) {
      words.push_back((SPD_VREG_WAIT<<16)|0xffff);
      w-=0xffff;
    }
    words.push_back((SPD_VREG_WAIT<<16)|w);
  }
  out.push_back(((unsigned int)SPD_PKT_REG_WRITE<<24)|(unsigned int)words.size());
  out.insert(out.end(),words.begin(),words.end());
  packets++;
  if (d==1) {
    out.push_back(((unsigned int)SPD_PKT_WAIT<<24)|1);
    out.push_back(0);
    packets++;
  }
}

void DivExportSPUDump::run() {
  bool loop=conf.getBool("loop",true);
  bool clearReverb=conf.getBool("clearReverb",true);

  int sys=-1;
  for (int i=0; i<e->song.systemLen; i++) {
    if (e->song.system[i]==DIV_SYSTEM_PS1) {
      sys=i;
      break;
    }
  }
  if (sys<0) {
    logAppend("ERROR: no PS1 SPU in this song");
    failed=true;
    running=false;
    return;
  }
  DivPlatformPS1* ps1=(DivPlatformPS1*)e->getDispatch(sys);

  std::vector<SPDPattern> pats;
  int loopPat=0;
  unsigned int initRate=0;
  unsigned short shadow[256];
  bool shadowSet[256];
  memset(shadow,0,sizeof(shadow));
  memset(shadowSet,0,sizeof(shadowSet));
  unsigned int voicesUsed=0;
  size_t totalTicks=0;

  e->stop();
  e->repeatPattern=false;
  e->setOrder(0);
  e->synchronizedSoft([&]() {
    e->calcSongTimestamps();
    int loopOrder=e->curSubSong->ts.loopStart.order;
    int loopRow=e->curSubSong->ts.loopStart.row;
    logAppendf("loop point: order %d row %d",loopOrder,loopRow);

    e->curOrder=0;
    e->freelance=false;
    e->playing=false;
    e->extValuePresent=false;
    e->remainingLoops=-1;
    e->playSub(false);
    initRate=(unsigned int)round(e->divider*65536.0);

    DivDispatch* disp=e->disCont[sys].dispatch;
    disp->toggleRegisterDump(true);

    bool loopPlaced=false;
    bool loopNow=false;
    int ordersLen=MAX(1,e->curSubSong->ordersLen);
    while (!mustAbort) {
      // a new pattern starts with the tick that plays a new row of another order, or at the
      // loop point (the tick right after it if virtual tempo skips the exact row).
      bool newPat=pats.empty();
      bool atLoop=false;
      if (e->ticks<=1 && !pats.empty() && e->curOrder!=pats.back().order) newPat=true;
      if (!loopPlaced) {
        if (loopOrder==e->curOrder && loopRow==e->curRow) loopNow=true;
        if (loopNow && (e->ticks<=1 || !(loopOrder==e->curOrder && loopRow==e->curRow))) {
          newPat=true;
          atLoop=true;
          loopPlaced=true;
          loopNow=false;
        }
      }
      if (newPat) {
        if (pats.empty() || !pats.back().ticks.empty()) {
          SPDPattern p;
          p.order=e->curOrder;
          // reverb unit (mBASE, then its registers), volumes and enables, SPUCNT, voices
          if (shadowSet[0x1a2>>1]) p.snapshot.push_back((0x1a2U<<16)|shadow[0x1a2>>1]);
          for (unsigned int r=0x1c0; r<0x200; r+=2) {
            if (shadowSet[r>>1]) p.snapshot.push_back((r<<16)|shadow[r>>1]);
          }
          for (unsigned int r=0x180; r<0x1c0; r+=2) {
            if (r==0x1a2 || r==0x1aa) continue;
            if (snapshotReg(r) && shadowSet[r>>1]) p.snapshot.push_back((r<<16)|shadow[r>>1]);
          }
          if (shadowSet[0x1aa>>1]) p.snapshot.push_back((0x1aaU<<16)|shadow[0x1aa>>1]);
          for (unsigned int r=0; r<0x180; r+=2) {
            if (snapshotReg(r) && shadowSet[r>>1]) p.snapshot.push_back((r<<16)|shadow[r>>1]);
          }
          pats.push_back(p);
          progress[0].amount=MIN(1.0f,(float)e->curOrder/(float)ordersLen);
        } else {
          pats.back().order=e->curOrder;
        }
      }
      if (atLoop) loopPat=(int)pats.size()-1;

      bool done=(e->nextTick() || !e->playing);
      std::vector<DivRegWrite>& writes=disp->getRegisterWrites();
      if (done) {
        // the tick that wrapped around is the loop target's first tick: not recorded.
        writes.clear();
        break;
      }
      SPDTick t;
      t.rate=(unsigned int)round(e->divider*65536.0);
      for (DivRegWrite& i: writes) {
        unsigned int addr=i.addr&0x1fe;
        unsigned int val=i.val&0xffff;
        t.words.push_back((addr<<16)|val);
        if (snapshotReg(addr)) {
          shadow[addr>>1]=val;
          shadowSet[addr>>1]=true;
        }
        if (addr<0x180) voicesUsed|=1U<<(addr>>4);
        if (addr==SPD_KON_LO || addr==SPD_KOFF_LO) voicesUsed|=val;
        if (addr==SPD_KON_HI || addr==SPD_KOFF_HI) voicesUsed|=(val&0xff)<<16;
      }
      writes.clear();
      pats.back().ticks.push_back(t);
      totalTicks++;
    }

    disp->toggleRegisterDump(false);
    e->remainingLoops=-1;
    e->playing=false;
    e->freelance=false;
    e->extValuePresent=false;
  });

  if (mustAbort) {
    logAppend("aborted");
    failed=true;
    running=false;
    return;
  }
  // drop a trailing pattern with no ticks
  while (!pats.empty() && pats.back().ticks.empty()) pats.pop_back();
  if (pats.empty()) {
    logAppend("ERROR: the song produced no ticks");
    failed=true;
    running=false;
    return;
  }
  if (loopPat>=(int)pats.size()) loopPat=0;
  if (pats.size()>SPD_MAX_PATTERNS) {
    logAppendf("WARNING: %d patterns; spdplayer plays at most %d",(int)pats.size(),SPD_MAX_PATTERNS);
  }
  int voiceCount=0;
  for (int i=0; i<24; i++) {
    if (voicesUsed&(1U<<i)) voiceCount=i+1;
  }

  // pattern streams
  std::vector<std::vector<unsigned int>> streams;
  size_t packets=0;
  unsigned int curRate=initRate;
  for (size_t pi=0; pi<pats.size(); pi++) {
    SPDPattern& p=pats[pi];
    std::vector<unsigned int> out;
    std::vector<unsigned int> batch=p.snapshot;
    size_t n=p.ticks.size();
    size_t lastTick=0;
    bool havePending=false;
    for (size_t ti=0; ti<n; ti++) {
      SPDTick& t=p.ticks[ti];
      bool rateChange=(t.rate!=curRate);
      if (ti==0 || !t.words.empty() || rateChange) {
        if (havePending) {
          emitBatch(out,batch,(unsigned int)(ti-lastTick),packets);
          batch.clear();
        }
        if (rateChange) {
          // takes effect for the gap after this tick, as in the engine
          out.push_back(((unsigned int)SPD_PKT_TICK_RATE<<24)|1);
          out.push_back(t.rate);
          packets++;
          curRate=t.rate;
        }
        batch.insert(batch.end(),t.words.begin(),t.words.end());
        lastTick=ti;
        havePending=true;
      }
    }
    unsigned int d=(unsigned int)(n-lastTick);
    emitBatch(out,batch,d,packets);
    if (pi==pats.size()-1 && !loop && voiceCount>0) {
      // end of a non-looping song: release every voice the stream used
      unsigned int mask=(voiceCount>=24)?0xffffff:((1U<<voiceCount)-1);
      out.push_back(((unsigned int)SPD_PKT_REG_WRITE<<24)|((mask>>16)?2:1));
      out.push_back((SPD_KOFF_LO<<16)|(mask&0xffff));
      if (mask>>16) out.push_back((SPD_KOFF_HI<<16)|(mask>>16));
      packets++;
    }
    out.push_back((unsigned int)SPD_PKT_END_PATTERN<<24);
    packets++;
    streams.push_back(out);
  }

  // song file
  SafeWriter* w=new SafeWriter;
  w->init();
  writeMagic(w);
  size_t headerPackets=0;
  if (!e->song.name.empty()) {
    writeString(w,SPD_PKT_TITLE,e->song.name);
    headerPackets++;
  }
  if (!e->song.author.empty()) {
    writeString(w,SPD_PKT_AUTHOR,e->song.author);
    headerPackets++;
  }
  writeString(w,SPD_PKT_COMMENT,"Furnace " DIV_VERSION " SPUDUMP export");
  writeWord(w,SPD_PKT_TICK_RATE,initRate);
  writeWord(w,SPD_PKT_VOICE_COUNT,(unsigned int)voiceCount);
  headerPackets+=3;
  writeHeader(w,SPD_PKT_ORDER_TABLE,2+(unsigned int)pats.size());
  w->writeI(loop?1:0);
  w->writeI(loop?(unsigned int)loopPat:0);
  for (size_t i=0; i<pats.size(); i++) w->writeI((unsigned int)i);
  headerPackets++;
  size_t hdrPos=w->tell();
  size_t streamPos=hdrPos+pats.size()*8;
  for (size_t i=0; i<pats.size(); i++) {
    writeHeader(w,SPD_PKT_PATTERN_HDR,1);
    w->writeI((unsigned int)streamPos);
    streamPos+=streams[i].size()*4;
  }
  headerPackets+=pats.size();
  for (std::vector<unsigned int>& s: streams) {
    for (unsigned int i: s) w->writeI(i);
  }

  // sample bank file
  SafeWriter* wb=new SafeWriter;
  wb->init();
  writeMagic(wb);
  size_t bankPackets=0;
  const unsigned char* mem=(const unsigned char*)ps1->getSampleMem(0);
  size_t memLen=ps1->getSampleMemUsage(0);
  unsigned int base=ps1->getSampleBase();
  const DivMemoryComposition* mc=ps1->getMemCompo(0);
  {
    int count=0;
    std::vector<unsigned int> dir;
    if (mc!=NULL) {
      for (const DivMemoryEntry& m: mc->entries) {
        if (m.type!=DIV_MEMORY_SAMPLE) continue;
        if (m.asset<0 || m.asset>=SPD_MAX_SAMPLES) continue;
        if (m.asset+1>count) count=m.asset+1;
      }
    }
    dir.resize(count*2,0);
    if (mc!=NULL && mem!=NULL) {
      for (const DivMemoryEntry& m: mc->entries) {
        if (m.type!=DIV_MEMORY_SAMPLE) continue;
        if (m.asset<0 || m.asset>=SPD_MAX_SAMPLES) continue;
        unsigned int start=(unsigned int)m.begin, end=(unsigned int)m.end;
        // loop: the last block carries loop end + repeat; the start is the block flagged
        // loop start (as the voice's repeat address follows it), else the sample start.
        bool hasLoop=false;
        unsigned int loopAddr=start;
        if (end>=start+16) {
          hasLoop=((mem[end-16+1]&3)==3);
          for (unsigned int b=start; b<end; b+=16) {
            if (mem[b+1]&4) {
              loopAddr=b;
              break;
            }
          }
        }
        dir[m.asset*2]=((start>>3)<<16)|(((end-start)>>3)&0xffff);
        dir[m.asset*2+1]=((loopAddr>>3)<<16)|(hasLoop?1:0);
      }
    }
    if (count>0) {
      writeHeader(wb,SPD_PKT_SAMPLE_DIR,1+count*2);
      wb->writeI(count);
      for (unsigned int i: dir) wb->writeI(i);
      bankPackets++;
    }
  }
  if (mem!=NULL && memLen>base) {
    writeSampleData(wb,base,mem+base,memLen-base);
    bankPackets++;
  }
  unsigned int rvbBase=ps1->getReverbBase();
  if (clearReverb && rvbBase<PS1SPUBridge::RAM_SIZE) {
    writeSampleData(wb,rvbBase,NULL,PS1SPUBridge::RAM_SIZE-rvbBase);
    bankPackets++;
  }

  logAppendf("%d patterns (loop to %d), %d ticks, %d voices",(int)pats.size(),loop?loopPat:-1,(int)totalTicks,voiceCount);
  logAppendf("out.spudump: %d bytes, %d packets",(int)w->size(),(int)(headerPackets+packets));
  logAppendf("out.spubank: %d bytes, %d packets",(int)wb->size(),(int)bankPackets);
  progress[0].amount=1.0f;
  logAppend("finished!");

  output.push_back(DivROMExportOutput("out.spudump",w));
  output.push_back(DivROMExportOutput("out.spubank",wb));
  running=false;
}

bool DivExportSPUDump::go(DivEngine* eng) {
  progress[0].name="Export";
  progress[0].amount=0.0f;
  e=eng;
  running=true;
  failed=false;
  mustAbort=false;
  exportThread=new std::thread(&DivExportSPUDump::run,this);
  return true;
}

bool DivExportSPUDump::isRunning() {
  return running;
}

bool DivExportSPUDump::hasFailed() {
  return failed;
}

void DivExportSPUDump::abort() {
  mustAbort=true;
  wait();
}

void DivExportSPUDump::wait() {
  if (exportThread!=NULL) {
    exportThread->join();
    delete exportThread;
    exportThread=NULL;
  }
}

DivROMExportProgress DivExportSPUDump::getProgress(int index) {
  if (index<0 || index>1) return progress[0];
  return progress[index];
}
