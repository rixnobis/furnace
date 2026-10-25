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
// the loop: the platform skips writes its register cache already holds, so the song entered
// from its own end can write differently than from a cold start. the export plays the loop
// twice and appends the loop-back patterns whose effect differs, looping into the first pass's
// copies once the two agree in effect and register state.

#include "spudump.h"
#include "../engine.h"
#include "../platform/ps1.h"
#include "../../ta-log.h"
#include <fmt/printf.h>
#include <algorithm>
#include <functional>
#include <map>

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
#define SPD_PKT_MACRO_DEF 0x13
#define SPD_MAX_MACROS 256
#define SPD_MAX_MACRO_WRITES 8

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

// the replay's register state, as far as a write can be redundant: a write to a snapshot
// register holding that value already changes nothing. KON/KOFF and the rest always count.
struct SPDRegState {
  unsigned short v[256];
  bool set[256];
  SPDRegState() {
    memset(v,0,sizeof(v));
    memset(set,0,sizeof(set));
  }
  bool operator==(const SPDRegState& o) const {
    for (int i=0; i<256; i++) {
      if (set[i]!=o.set[i] || (set[i] && v[i]!=o.v[i])) return false;
    }
    return true;
  }
};

// play a pattern from state st: the writes that take effect, tick by tick (the snapshot opens
// tick 0, as it is emitted), with the tick rate
static std::vector<std::vector<unsigned int>> spdEffective(const SPDPattern& p, SPDRegState& st) {
  std::vector<std::vector<unsigned int>> eff;
  auto apply=[&](std::vector<unsigned int>& out, unsigned int w) {
    unsigned int r=w>>16, val=w&0xffff;
    if (snapshotReg(r)) {
      if (st.set[r>>1] && st.v[r>>1]==val) return;
      st.v[r>>1]=val;
      st.set[r>>1]=true;
    }
    out.push_back(w);
  };
  for (size_t ti=0; ti<p.ticks.size(); ti++) {
    std::vector<unsigned int> out;
    out.push_back(p.ticks[ti].rate);
    if (ti==0) for (unsigned int w: p.snapshot) apply(out,w);
    for (unsigned int w: p.ticks[ti].words) apply(out,w);
    eff.push_back(out);
  }
  return eff;
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
  // the loop-back pass, captured as the engine plays it: the platform skips writes whose value
  // its register cache already holds, so a pattern can write differently when entered from the
  // end of the song than from a cold start
  std::vector<SPDPattern> pats2;
  std::vector<SPDPattern>* cur=&pats;
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
      bool newPat=cur->empty();
      bool atLoop=false;
      if (e->ticks<=1 && !cur->empty() && e->curOrder!=cur->back().order) newPat=true;
      if (!loopPlaced && cur==&pats) {
        if (loopOrder==e->curOrder && loopRow==e->curRow) loopNow=true;
        if (loopNow && (e->ticks<=1 || !(loopOrder==e->curOrder && loopRow==e->curRow))) {
          newPat=true;
          atLoop=true;
          loopPlaced=true;
          loopNow=false;
        }
      }
      if (newPat) {
        if (cur->empty() || !cur->back().ticks.empty()) {
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
          cur->push_back(p);
          progress[0].amount=MIN(1.0f,(float)e->curOrder/(float)ordersLen);
        } else {
          cur->back().order=e->curOrder;
        }
      }
      if (atLoop) loopPat=(int)pats.size()-1;

      bool wrapped=e->nextTick();
      bool done=(wrapped || !e->playing);
      std::vector<DivRegWrite>& writes=disp->getRegisterWrites();
      if (done && !(loop && wrapped && e->playing && cur==&pats)) {
        // the tick that wrapped around is the loop target's first tick: not recorded.
        writes.clear();
        break;
      }
      if (done) {
        // end of the first pass: the wrapped tick opens the loop-back pass
        cur=&pats2;
        SPDPattern p;
        p.order=e->curOrder;
        cur->push_back(p);
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
      cur->back().ticks.push_back(t);
      if (cur==&pats) totalTicks++;
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
  // loop-back pass: where it takes a different effect from the first pass, append its differing
  // head (loop target up to where the passes agree to the end) and loop to the first pass's
  // copy of the rest: [P0..Pn, PT'..Pj-1'], loop target j
  while (!pats2.empty() && pats2.back().ticks.empty()) pats2.pop_back();
  if (loop && !pats2.empty()) {
    size_t n2=pats.size()-(size_t)loopPat;
    if (pats2.size()!=n2) {
      logAppendf("WARNING: loop-back pass has %d patterns, first pass %d from the loop; looping to the first pass's",(int)pats2.size(),(int)n2);
    } else {
      // first-pass pattern k can stand in for loop-back pattern k when, played from the state
      // the loop-back pass enters k with, it takes the same effect and leaves the same state
      SPDRegState st;
      for (SPDPattern& p: pats) spdEffective(p,st);
      SPDRegState end1=st;
      size_t j=0;
      for (size_t k=0; k<n2; k++) {
        SPDRegState alt=st;
        std::vector<std::vector<unsigned int>> e2=spdEffective(pats2[k],st);
        std::vector<std::vector<unsigned int>> e1=spdEffective(pats[loopPat+k],alt);
        if (!(e1==e2 && alt==st)) j=k+1;
      }
      if (!(st==end1)) {
        logAppend("WARNING: register state at the end of the loop-back pass differs from the first pass's; later passes are not checked");
      }
      if (j>0) {
        logAppendf("loop-back pass takes a different effect in its first %d of %d patterns; appended",(int)j,(int)n2);
        int oldLoop=loopPat;
        for (size_t i=0; i<j; i++) pats.push_back(pats2[i]);
        loopPat=(j<n2)?(oldLoop+(int)j):(int)(pats.size()-j);
      }
    }
  }
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

  // macros (0x13): a run of consecutive writes to one voice inside a register write packet,
  // seen often enough, is defined once (voice 0 relative) and invoked as one 0xF000|index
  // word. only whole consecutive runs are replaced, so the SPU sees the same writes in the
  // same order.
  std::vector<std::vector<unsigned int>> macroDefs;
  {
    auto forEachRun=[&](std::vector<unsigned int>& s, const std::function<void(size_t,size_t)>& f) {
      size_t i=0;
      while (i<s.size()) {
        unsigned int type=s[i]>>24, len=s[i]&0xffffff;
        if (type==SPD_PKT_REG_WRITE) {
          size_t a=i+1, e=i+1+len;
          while (a<e) {
            unsigned int off=s[a]>>16;
            if (off>=0x180) { a++; continue; }
            size_t b=a+1;
            while (b<e && b-a<SPD_MAX_MACRO_WRITES && (s[b]>>16)<0x180 && ((s[b]>>16)>>4)==(off>>4)) b++;
            if (b-a>=2) f(a,b);
            a=b;
          }
        }
        i+=1+len;
      }
    };
    auto keyOf=[](const std::vector<unsigned int>& s, size_t a, size_t b) {
      std::vector<unsigned int> k;
      for (size_t j=a; j<b; j++) k.push_back((((s[j]>>16)&0xf)<<16)|(s[j]&0xffff));
      return k;
    };
    std::map<std::vector<unsigned int>,size_t> uses;
    for (std::vector<unsigned int>& s: streams) {
      forEachRun(s,[&](size_t a, size_t b) { uses[keyOf(s,a,b)]++; });
    }
    std::vector<std::pair<long,std::vector<unsigned int>>> cand;
    for (auto& u: uses) {
      long save=(long)u.second*(long)(u.first.size()-1)-(long)(u.first.size()+2);
      if (save>0) cand.push_back(std::make_pair(save,u.first));
    }
    std::sort(cand.begin(),cand.end(),[](const std::pair<long,std::vector<unsigned int>>& x, const std::pair<long,std::vector<unsigned int>>& y) {
      return x.first>y.first || (x.first==y.first && x.second<y.second);
    });
    std::map<std::vector<unsigned int>,unsigned int> index;
    for (auto& c: cand) {
      if (macroDefs.size()>=SPD_MAX_MACROS) break;
      index[c.second]=(unsigned int)macroDefs.size();
      macroDefs.push_back(c.second);
    }
    if (!macroDefs.empty()) {
      for (std::vector<unsigned int>& s: streams) {
        std::vector<unsigned int> o;
        size_t i=0;
        while (i<s.size()) {
          unsigned int type=s[i]>>24, len=s[i]&0xffffff;
          if (type!=SPD_PKT_REG_WRITE) {
            o.insert(o.end(),s.begin()+i,s.begin()+i+1+len);
            i+=1+len;
            continue;
          }
          std::vector<unsigned int> payload(s.begin()+i+1,s.begin()+i+1+len);
          std::vector<unsigned int> np;
          std::vector<std::pair<size_t,size_t>> runs;
          std::vector<unsigned int> one(s.begin()+i,s.begin()+i+1+len);
          forEachRun(one,[&](size_t a, size_t b) { runs.push_back(std::make_pair(a-1,b-1)); });
          size_t r=0;
          for (size_t j=0; j<payload.size();) {
            if (r<runs.size() && runs[r].first==j) {
              std::vector<unsigned int> k=keyOf(payload,runs[r].first,runs[r].second);
              auto it=index.find(k);
              if (it!=index.end()) {
                np.push_back(((0xf000U|it->second)<<16)|((payload[j]>>16)>>4));
              } else {
                np.insert(np.end(),payload.begin()+runs[r].first,payload.begin()+runs[r].second);
              }
              j=runs[r].second;
              r++;
              continue;
            }
            np.push_back(payload[j]);
            j++;
          }
          o.push_back(((unsigned int)SPD_PKT_REG_WRITE<<24)|(unsigned int)np.size());
          o.insert(o.end(),np.begin(),np.end());
          i+=1+len;
        }
        s=o;
      }
    }
  }

  // identical streams share one pattern; the order table carries the playback order. only
  // when the tick rate never changes, since a stream's rate packets are relative to the rate
  // it is entered with.
  std::vector<unsigned int> orderPat;
  {
    bool rateMoves=false;
    for (SPDPattern& p: pats) for (SPDTick& t: p.ticks) if (t.rate!=initRate) rateMoves=true;
    std::vector<std::vector<unsigned int>> uniq;
    std::map<std::vector<unsigned int>,unsigned int> seen;
    for (std::vector<unsigned int>& s: streams) {
      auto it=rateMoves?seen.end():seen.find(s);
      if (it!=seen.end()) {
        orderPat.push_back(it->second);
      } else {
        unsigned int idx=(unsigned int)uniq.size();
        if (!rateMoves) seen[s]=idx;
        uniq.push_back(s);
        orderPat.push_back(idx);
      }
    }
    streams=uniq;
    packets=0;
    for (std::vector<unsigned int>& s: streams) {
      for (size_t i=0; i<s.size(); i+=1+(s[i]&0xffffff)) packets++;
    }
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
  for (size_t i=0; i<macroDefs.size(); i++) {
    writeHeader(w,SPD_PKT_MACRO_DEF,1+(unsigned int)macroDefs[i].size());
    w->writeI((unsigned int)i);
    for (unsigned int m: macroDefs[i]) w->writeI(m);
    headerPackets++;
  }
  writeHeader(w,SPD_PKT_ORDER_TABLE,2+(unsigned int)orderPat.size());
  w->writeI(loop?1:0);
  w->writeI(loop?(unsigned int)loopPat:0);
  for (unsigned int o: orderPat) w->writeI(o);
  headerPackets++;
  size_t hdrPos=w->tell();
  size_t streamPos=hdrPos+streams.size()*8;
  for (size_t i=0; i<streams.size(); i++) {
    writeHeader(w,SPD_PKT_PATTERN_HDR,1);
    w->writeI((unsigned int)streamPos);
    streamPos+=streams[i].size()*4;
  }
  headerPackets+=streams.size();
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

  logAppendf("%d orders, %d patterns (loop to %d), %d macros, %d ticks, %d voices",(int)orderPat.size(),(int)streams.size(),loop?loopPat:-1,(int)macroDefs.size(),(int)totalTicks,voiceCount);
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
