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

// Song for nugget's xmplayer (format version 2), written straight from a PS1 SPU song: out.xmp (song)
// and out.smp (samples, as the SPU plays them). Version 2 carries what FastTracker 2 cannot: the tick
// rate, reverb, each instrument's hardware ADSR, noise, pitch modulation and independent left/right
// levels. What still does not fit is counted and logged.

#include "xmp.h"
#include "../engine.h"
#include "../platform/ps1.h"
#include "../../ta-log.h"
#include <fmt/printf.h>
#include <math.h>
#include <map>
#include <vector>

namespace {

enum {
  L_EFFECT=0,
  L_EXTRA_FX,
  L_EXTRA_EXT,
  L_NOTEMAP,
  L_NOTE_RANGE,
  L_MACROS,
  L_GROOVE,
  L_SPEED,
  L_JUMP,
  L_MAX
};

const char* lossNames[L_MAX]={
  "effects with no xmplayer equivalent",
  "rows with more effects than the one effect column",
  "rows with more than one extended effect (levels and noise clock)",
  "note map entries that remap pitch",
  "notes outside C-0..B-7",
  "instruments with macros (not exported)",
  "groove (more than one speed)",
  "speeds above 31 ticks per row (clamped to 31)",
  "jumps (the target order is converted with the channel state of the order listed before it)",
};

struct Cell {
  int note=0, ins=0, vol=0, fx=0, fxv=0, ext=0, extv=0;
};

void wstr(SafeWriter* w, const String& s, size_t len) {
  for (size_t i=0; i<len; i++) w->writeC(i<s.size()?s[i]:0);
}

void align4(SafeWriter* w) {
  while (w->size()&3) w->writeC(0);
}

bool hasMacros(DivInstrument* ins) {
  DivInstrumentSTD& s=ins->std;
  return s.volMacro.len || s.arpMacro.len || s.pitchMacro.len || s.panLMacro.len || s.panRMacro.len;
}

}

void DivExportXMP::run() {
  int loss[L_MAX];
  memset(loss,0,sizeof(loss));
  int sys=-1;
  for (int i=0; i<e->song.systemLen; i++) {
    if (e->song.system[i]==DIV_SYSTEM_PS1) {
      sys=i;
      break;
    }
  }
  if (sys<0) {
    logAppend("ERROR: no PS1 SPU in this song");
    failed=true; running=false; return;
  }
  DivPlatformPS1* ps1=(DivPlatformPS1*)e->getDispatch(sys);
  DivSubSong* sub=e->curSubSong;
  int chans=MIN(24,e->getTotalChannelCount());
  int patLen=sub->patLen;
  if (patLen>256) {
    logAppendf("ERROR: %d rows per pattern, the player holds 256",patLen);
    failed=true; running=false; return;
  }
  if (sub->speeds.len>1) loss[L_GROOVE]=1;
  int speed=CLAMP(sub->speeds.val[0],1,31);
  int bpm=CLAMP((int)lrint(sub->hz*2.5),32,255);
  unsigned int tickRate=(unsigned int)lrint(sub->hz*65536.0);

  // samples: the bytes the platform uploads, one record per (instrument, sample) pair because the
  // ADSR and the voice flags live in the instrument
  const unsigned char* ram=(const unsigned char*)ps1->getSampleMem(0);
  const DivMemoryComposition* mc=ps1->getMemCompo(0);
  std::map<int,std::pair<size_t,size_t>> spuRange; // sample -> [begin, end) in SPU RAM
  size_t reverbBegin=0x80000;
  for (const DivMemoryEntry& m: mc->entries) {
    if (m.type==DIV_MEMORY_SAMPLE && m.asset>=0) spuRange[m.asset]=std::make_pair(m.begin,m.end);
    if (m.type==DIV_MEMORY_ECHO) reverbBegin=MIN(reverbBegin,m.begin);
  }
  SafeWriter* smp=new SafeWriter;
  smp->init();
  std::map<int,unsigned int> smpOffset;
  for (auto& r: spuRange) {
    smpOffset[r.first]=(unsigned int)smp->size();
    smp->write(ram+r.second.first,r.second.second-r.second.first);
  }
  while (smp->size()&63) smp->writeC(0);
  if (0x1010+smp->size()>reverbBegin) {
    logAppendf("WARNING: %d bytes of samples from 0x1010 reach the reverb work area at 0x%x",(int)smp->size(),(int)reverbBegin);
  }

  int insCount=MIN(128,e->song.insLen);
  struct SampleRec {
    int ins, smp;
  };
  std::vector<SampleRec> recs;
  std::vector<std::vector<unsigned char>> keymaps(insCount,std::vector<unsigned char>(96,0xff));
  int recsNeeded=0;
  for (int i=0; i<insCount; i++) {
    DivInstrument* ins=e->song.ins[i];
    if (hasMacros(ins)) loss[L_MACROS]++;
    std::map<int,int> local;
    for (int n=0; n<96; n++) {
      int fn=n+60;
      int s=ins->amiga.getSample(fn);
      if (ins->amiga.useNoteMap && ins->amiga.getFreq(fn)!=fn) loss[L_NOTEMAP]++;
      if (s<0 || spuRange.find(s)==spuRange.end()) continue;
      auto it=local.find(s);
      if (it==local.end()) {
        recsNeeded++;
        if (recs.size()>=255) { local[s]=-1; continue; }
        local[s]=(int)recs.size();
        recs.push_back(SampleRec{i,s});
        it=local.find(s);
      }
      if (it->second>=0) keymaps[i][n]=(unsigned char)it->second;
    }
  }
  if (recsNeeded>255) {
    logAppendf("ERROR: %d sample records needed (one per instrument and sample it maps), the player holds 255; map fewer samples per instrument or merge instruments that share samples",recsNeeded);
    failed=true; running=false; smp->finish(); delete smp; return;
  }

  // patterns: every order is converted on its own, with the channel state it is entered with in
  // song order (Furnace keeps volume and levels across orders), and orders that come out byte for
  // byte the same share one pattern
  std::map<std::vector<unsigned char>,int> patIndex;
  std::vector<std::vector<unsigned char>> patData;
  std::vector<int> order;
  std::vector<int> chVol(chans,127), chPanL(chans,255), chPanR(chans,255), chLevelL(chans,-1), chLevelR(chans,-1);
  // the noise clock a freshly reset platform starts with (not the live one, which a preview may
  // have changed), set on the first row of the song
  int noiseInit=PS1_NOISE_DEFAULT;
  const unsigned short* regs=(const unsigned short*)ps1->getRegisterPool();
  bool anyNoise=false;
  for (int i=0; i<insCount; i++) {
    if (e->song.ins[i]->type==DIV_INS_PS1 && e->song.ins[i]->ps1.noise) anyNoise=true;
  }
  if (!anyNoise) noiseInit=0;
  for (int o=0; o<sub->ordersLen; o++) {
    if (mustAbort) {
      logAppend("aborted");
      smp->finish();
      delete smp;
      failed=true; running=false; return;
    }
    std::vector<std::vector<Cell>> rows(patLen,std::vector<Cell>(chans));
    for (int c=0; c<chans; c++) {
      DivPattern* pat=sub->pat[c].getPattern(sub->orders.ord[c][o],false);
      int fxCols=sub->pat[c].effectCols;
      for (int r=0; r<patLen; r++) {
        Cell& cell=rows[r][c];
        short* d=pat->newData[r];
        int note=d[DIV_PAT_NOTE];
        bool newNote=false;
        if (note==DIV_NOTE_OFF || note==DIV_NOTE_REL || note==DIV_MACRO_REL) {
          cell.note=97;
        } else if (note>=0 && note<180) {
          int xn=note-60+1;
          if (xn<1 || xn>96) { loss[L_NOTE_RANGE]++; xn=CLAMP(xn,1,96); }
          cell.note=xn;
          newNote=true;
        }
        if (d[DIV_PAT_INS]>=0 && d[DIV_PAT_INS]<insCount) cell.ins=d[DIV_PAT_INS]+1;
        if (d[DIV_PAT_VOL]>=0) chVol[c]=d[DIV_PAT_VOL]&127;
        int keyOff=-1, otherFx=-1, otherFxv=0, noise=-1;
        for (int k=0; k<fxCols; k++) {
          int fx=d[DIV_PAT_FX(k)], fv=d[DIV_PAT_FXVAL(k)];
          if (fx<0) continue;
          if (fv<0) fv=0;
          switch (fx) {
            case 0x80: chPanL[c]=MIN(255,(255-fv)*2); chPanR[c]=MIN(255,fv*2); break;
            case 0x81: chPanL[c]=fv; break;
            case 0x82: chPanR[c]=fv; break;
            case 0xfc: keyOff=fv; break;
            case 0x1d: noise=fv&63; break;
            case 0x0f:
              // Furnace ignores 0F00; F20 and up would be read as BPM
              if (fv==0) break;
              if (fv>31) { loss[L_SPEED]++; fv=31; }
              if (otherFx>=0) loss[L_EXTRA_FX]++;
              otherFx=0x0f; otherFxv=fv;
              break;
            case 0x0d:
              if (otherFx>=0) loss[L_EXTRA_FX]++;
              otherFx=0x0d; otherFxv=((fv/10)<<4)|(fv%10);
              loss[L_JUMP]++;
              break;
            case 0x0b: if (otherFx>=0) loss[L_EXTRA_FX]++; otherFx=0x0b; otherFxv=fv; loss[L_JUMP]++; break;
            default: loss[L_EFFECT]++; break;
          }
        }
        if (keyOff>=0) {
          cell.fx=0x14; cell.fxv=keyOff;
          if (otherFx>=0) loss[L_EXTRA_FX]++;
        } else if (otherFx>=0) {
          cell.fx=otherFx; cell.fxv=otherFxv;
        }
        // the volume rides in the levels (8 bits a side) and the player's own volume stays at the
        // sample's 64: the volume column's 0..64 would halve the resolution of 0..127, which on a
        // quiet note is a factor of two. The first note of a channel sets them even when nothing
        // changed, so the channel leaves the pan law for good.
        int lvL=(int)lrint(chPanL[c]*chVol[c]/127.0), lvR=(int)lrint(chPanR[c]*chVol[c]/127.0);
        if (chLevelL[c]<0 ? newNote : (lvL!=chLevelL[c] || lvR!=chLevelR[c])) {
          cell.ext=1; cell.extv=lvL|(lvR<<8);
          chLevelL[c]=lvL; chLevelR[c]=lvR;
          if (noise>=0) loss[L_EXTRA_EXT]++;
        } else if (noise>=0) {
          cell.ext=2; cell.extv=noise;
        }
      }
    }
    if (o==0 && noiseInit && patLen>0) {
      // the starting noise clock, on the first row's first free extended slot
      int c=0;
      while (c<chans && rows[0][c].ext) c++;
      if (c<chans) {
        rows[0][c].ext=2;
        rows[0][c].extv=noiseInit;
      } else {
        loss[L_EXTRA_EXT]++;
      }
    }
    std::vector<unsigned char> pb;
    for (int r=0; r<patLen; r++) {
      for (int c=0; c<chans; c++) {
        Cell& cl=rows[r][c];
        int mask=0x80|(cl.note?1:0)|(cl.ins?2:0)|(cl.vol?4:0)|(cl.fx?8:0)|(cl.fxv?16:0)|(cl.ext?32:0);
        pb.push_back((unsigned char)mask);
        if (cl.note) pb.push_back((unsigned char)cl.note);
        if (cl.ins) pb.push_back((unsigned char)cl.ins);
        if (cl.vol) pb.push_back((unsigned char)cl.vol);
        if (cl.fx) pb.push_back((unsigned char)cl.fx);
        if (cl.fxv) pb.push_back((unsigned char)cl.fxv);
        if (cl.ext) {
          pb.push_back((unsigned char)cl.ext);
          pb.push_back((unsigned char)(cl.extv&0xff));
          pb.push_back((unsigned char)((cl.extv>>8)&0xff));
        }
      }
    }
    auto it=patIndex.find(pb);
    if (it==patIndex.end()) {
      patIndex[pb]=(int)patData.size();
      order.push_back((int)patData.size());
      patData.push_back(pb);
    } else {
      order.push_back(it->second);
    }
    progressAmount=0.9f*(o+1)/sub->ordersLen;
  }
  if (order.size()>255 || patData.size()>256) {
    logAppendf("ERROR: %d orders, %d patterns; the player holds 255 and 256",(int)order.size(),(int)patData.size());
    failed=true; running=false; smp->finish(); delete smp; return;
  }

  // song file
  SafeWriter* w=new SafeWriter;
  w->init();
  w->write("PSXM",4);
  w->writeS(2);
  w->writeS(72);
  w->writeC(chans);
  w->writeC(1); // linear periods
  w->writeC(speed);
  w->writeC(bpm);
  w->writeS((int)order.size());
  w->writeS(0); // restart
  w->writeS((int)patData.size());
  w->writeS(insCount);
  w->writeS((int)recs.size());
  w->writeS(0); // no ProTracker period table
  w->writeI((int)smp->size());
  size_t offsetsAt=w->size();
  for (int i=0; i<4; i++) w->writeI(0);
  wstr(w,e->song.name,20);
  w->writeI((int)tickRate);
  size_t reverbAt=w->size();
  w->writeI(0);

  size_t ordersOffset=w->size();
  for (int o: order) w->writeC(o);
  align4(w);
  size_t patternsOffset=w->size();
  size_t dataAt=patternsOffset+patData.size()*8;
  for (size_t p=0; p<patData.size(); p++) {
    w->writeI((int)dataAt);
    w->writeS(patLen);
    w->writeS((int)patData[p].size());
    dataAt+=patData[p].size();
  }
  for (const std::vector<unsigned char>& pb: patData) {
    if (!pb.empty()) w->write(pb.data(),pb.size());
  }
  align4(w);
  size_t instrumentsOffset=w->size();
  for (int i=0; i<insCount; i++) {
    w->write(keymaps[i].data(),96);
    for (int k=0; k<96; k++) w->writeC(0); // no software envelopes
    for (int k=0; k<10; k++) w->writeC(0);
    for (int k=0; k<4; k++) w->writeC(0); // no auto vibrato
    w->writeS(0); // no fadeout: the hardware release ends the note
  }
  size_t samplesOffset=w->size();
  for (const SampleRec& sr: recs) {
    DivInstrument* ins=e->song.ins[sr.ins];
    DivSample* s=e->song.sample[sr.smp];
    size_t spuLen=spuRange[sr.smp].second-spuRange[sr.smp].first;
    double semis=12.0*log2((double)MAX(1,s->centerRate)/8363.0);
    int rel=(int)floor(semis);
    // linear periods use finetune/2 (64 steps per semitone), so an odd finetune is truncated: round
    // to the nearest 1/64 semitone and write it even
    int ft=2*(int)lrint((semis-rel)*64.0);
    if (ft>127) { ft-=128; rel++; }
    if (rel>127 || rel<-128) loss[L_NOTE_RANGE]++;
    bool loop=s->loop && s->loopEnd>s->loopStart;
    unsigned int flags=(loop?1:0);
    unsigned int adsr=0x000f;
    if (ins->type==DIV_INS_PS1) {
      flags|=4;
      if (ins->ps1.reverb) flags|=8;
      if (ins->ps1.noise) flags|=16;
      if (ins->ps1.pitchMod) flags|=32;
      adsr=ins->ps1.getADSR1()|((unsigned int)ins->ps1.getADSR2()<<16);
    }
    w->writeI((int)smpOffset[sr.smp]);
    w->writeI((int)spuLen);
    w->writeI((int)s->samples);
    w->writeI(loop?s->loopStart:0);
    w->writeI(loop?(s->loopEnd-s->loopStart):0);
    w->writeS(0); // pad
    w->writeC(flags);
    w->writeC(0); // rate shift
    w->writeC(64);
    w->writeC((signed char)ft);
    w->writeC(128);
    w->writeC((signed char)CLAMP(rel,-128,127));
    w->writeI(adsr);
  }
  // reverb, when the platform has it on
  unsigned short mbase=regs[(0xda2-0xc00)>>1];
  if (mbase) {
    size_t at=w->size();
    for (int i=0; i<32; i++) w->writeS(regs[((0xdc0-0xc00)>>1)+i]);
    w->writeS(mbase);
    w->writeS(regs[(0xd84-0xc00)>>1]);
    w->writeS(regs[(0xd86-0xc00)>>1]);
    w->writeS(0);
    w->seek(reverbAt,SEEK_SET);
    w->writeI((int)at);
    w->seek(0,SEEK_END);
  }
  w->seek(offsetsAt,SEEK_SET);
  w->writeI((int)ordersOffset);
  w->writeI((int)patternsOffset);
  w->writeI((int)instrumentsOffset);
  w->writeI((int)samplesOffset);
  w->seek(0,SEEK_END);

  logAppendf("%d channels, %d orders, %d patterns, %d instruments, %d sample records, tick rate %.6f Hz, speed %d",chans,(int)order.size(),(int)patData.size(),insCount,(int)recs.size(),tickRate/65536.0,speed);
  logAppendf("out.xmp: %d bytes, out.smp: %d bytes%s",(int)w->size(),(int)smp->size(),mbase?", reverb on":"");
  int total=0;
  for (int i=0; i<L_MAX; i++) {
    if (!loss[i]) continue;
    logAppendf("LOSS %s: %d",lossNames[i],loss[i]);
    total++;
  }
  if (!total) logAppend("no losses");
  if (mustAbort) {
    logAppend("aborted");
    w->finish();
    delete w;
    smp->finish();
    delete smp;
    failed=true; running=false; return;
  }
  progressAmount=1.0f;
  logAppend("finished!");
  output.push_back(DivROMExportOutput("out.xmp",w));
  output.push_back(DivROMExportOutput("out.smp",smp));
  running=false;
}

bool DivExportXMP::go(DivEngine* eng) {
  progress[0].name="Export";
  progress[0].amount=0.0f;
  progressAmount=0.0f;
  e=eng;
  running=true;
  failed=false;
  mustAbort=false;
  exportThread=new std::thread(&DivExportXMP::run,this);
  return true;
}

bool DivExportXMP::isRunning() {
  return running;
}

bool DivExportXMP::hasFailed() {
  return failed;
}

void DivExportXMP::abort() {
  mustAbort=true;
  wait();
}

void DivExportXMP::wait() {
  if (exportThread!=NULL) {
    exportThread->join();
    delete exportThread;
    exportThread=NULL;
  }
}

DivROMExportProgress DivExportXMP::getProgress(int index) {
  if (index<0 || index>1) return progress[1];
  DivROMExportProgress p=progress[index];
  if (index==0) p.amount=progressAmount;
  return p;
}
