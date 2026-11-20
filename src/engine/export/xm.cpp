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

// FastTracker 2 module (.xm 0104) from a PS1 SPU song, for nugget's xmplayer (converted by Redux's
// xmconv). Plain XM cannot carry everything this chip does; every place the export has to
// approximate or drop something is counted and reported, so the log is the list of what does not
// survive.

#include "xm.h"
#include "../engine.h"
#include "../../ta-log.h"
#include <fmt/printf.h>
#include <math.h>
#include <map>
#include <vector>

namespace {

struct Loss {
  const char* what;
  int count;
};

enum {
  L_TICKRATE=0,
  L_GROOVE,
  L_ADSR,
  L_RELEASE,
  L_REVERB,
  L_NOISE,
  L_PMOD,
  L_REENCODE,
  L_PAN_DROPPED,
  L_PAN_COARSE,
  L_EFFECT,
  L_EXTRA_FX,
  L_NOTEMAP,
  L_NOTE_RANGE,
  L_SHARED_SAMPLE,
  L_VOL_QUANT,
  L_MAX
};

const char* lossNames[L_MAX]={
  "tick rate above XM's 255 BPM (102 Hz)",
  "groove (more than one speed)",
  "instruments whose hardware ADSR became a 12-point volume envelope",
  "instruments whose ADSR release became a linear fadeout",
  "instruments with reverb send (no XM field)",
  "instruments with the noise generator (no XM field)",
  "instruments with pitch modulation (no XM field)",
  "SPU ADPCM samples decoded to PCM (xmconv re-encodes them)",
  "rows where a pan change had no free column and was dropped",
  "rows where pan went to the volume column (16 steps)",
  "effects with no XM equivalent",
  "rows with more effects than XM's one column",
  "note map entries that remap pitch",
  "notes outside XM's C-0..B-7",
  "samples written once per instrument that uses them",
  "volume changes that round to the same XM volume as a different Furnace volume",
};

// PS1 ADSR, per sample at 44100 Hz, sampled every tick: a 12-point envelope is the best plain XM
// can do. Rates follow psx-spx (shift/step, exponential flags); this is an approximation for the
// export only, the core's envelope is the reference.
struct ADSRSim {
  int level;
  int phase; // 0 attack, 1 decay, 2 sustain
  int counter;
  unsigned short a1, a2;
  void start(unsigned short adsr1, unsigned short adsr2) {
    a1=adsr1; a2=adsr2; level=0; phase=0; counter=0;
  }
  static int stepOnce(int& counter, int shift, int step, bool exp, bool dec, int lvl) {
    int cycles=1<<MAX(0,shift-11);
    int s=step<<MAX(0,11-shift);
    if (exp && !dec && lvl>0x6000) cycles*=4;
    if (exp && dec) s=(s*lvl)>>15;
    if (++counter<cycles) return 0;
    counter=0;
    return s;
  }
  void run(int samples) {
    for (int i=0; i<samples; i++) {
      if (phase==0) {
        int shift=(a1>>10)&31, step=7-((a1>>8)&3);
        level+=stepOnce(counter,shift,step,a1&0x8000,false,level);
        if (level>=0x7fff) { level=0x7fff; phase=1; counter=0; }
      } else if (phase==1) {
        int shift=(a1>>4)&15;
        level-=stepOnce(counter,shift<<2,8,true,true,level);
        if (level<0) level=0;
        int sl=((a1&15)+1)<<11;
        if (level<=sl) { phase=2; counter=0; }
      } else {
        bool dec=a2&0x4000;
        int shift=(a2>>8)&31, step=dec?(8-((a2>>6)&3)):(7-((a2>>6)&3));
        int d=stepOnce(counter,shift,step,a2&0x8000,dec,level);
        level+=dec?-d:d;
        if (level<0) level=0;
        if (level>0x7fff) level=0x7fff;
      }
    }
  }
};

void w8(SafeWriter* w, int v) { w->writeC(v); }
void w16(SafeWriter* w, int v) { w->writeS(v); }
void w32(SafeWriter* w, int v) { w->writeI(v); }
void wstr(SafeWriter* w, const String& s, size_t len) {
  for (size_t i=0; i<len; i++) w->writeC(i<s.size()?s[i]:0);
}

struct Cell {
  int note=0, ins=0, vol=0, fx=0, fxv=0;
};

}

void DivExportXM::run() {
  int loss[L_MAX];
  memset(loss,0,sizeof(loss));
  DivSubSong* sub=e->song.subsong[0];
  int chans=e->getTotalChannelCount();
  if (chans>32) {
    logAppendf("ERROR: %d channels, XM holds 32",chans);
    failed=true; running=false; return;
  }
  int xmChans=(chans+1)&~1;
  int patLen=sub->patLen;
  if (patLen>256) {
    logAppendf("ERROR: %d rows per pattern, XM holds 256",patLen);
    failed=true; running=false; return;
  }

  // tick rate: XM ticks at BPM*2/5 Hz
  double bpmF=sub->hz*2.5;
  int bpm=(int)lrint(bpmF);
  if (bpm>255) { loss[L_TICKRATE]=1; logAppendf("tick rate %.4f Hz needs BPM %.2f; clamped to 255 (%.1f%% slow)",sub->hz,bpmF,100.0*(1.0-255.0/bpmF)); bpm=255; }
  if (bpm<32) { loss[L_TICKRATE]=1; bpm=32; }
  if (sub->speeds.len>1) loss[L_GROOVE]=1;
  int speed=sub->speeds.val[0];

  // instruments: one XM instrument per Furnace instrument; its samples are the ones it maps.
  int insCount=MIN(128,e->song.insLen);
  SafeWriter* w=new SafeWriter;
  w->init();
  w->write("Extended Module: ",17);
  wstr(w,e->song.name,20);
  w8(w,0x1a);
  wstr(w,"Furnace PS1 export",20);
  w16(w,0x104);
  w32(w,276);

  // patterns: one per distinct tuple of channel patterns
  std::map<std::vector<int>,int> patIndex;
  std::vector<std::vector<int>> patTuples;
  std::vector<int> order;
  for (int o=0; o<sub->ordersLen; o++) {
    std::vector<int> t(chans);
    for (int c=0; c<chans; c++) t[c]=sub->orders.ord[c][o];
    auto it=patIndex.find(t);
    if (it==patIndex.end()) {
      patIndex[t]=(int)patTuples.size();
      order.push_back((int)patTuples.size());
      patTuples.push_back(t);
    } else {
      order.push_back(it->second);
    }
  }
  if (order.size()>256 || patTuples.size()>256) {
    logAppendf("ERROR: %d orders, %d patterns; XM holds 256 of each",(int)order.size(),(int)patTuples.size());
    failed=true; running=false; delete w; return;
  }
  w16(w,(int)order.size());
  w16(w,0); // restart
  w16(w,xmChans);
  w16(w,(int)patTuples.size());
  w16(w,insCount);
  w16(w,1); // linear frequency table
  w16(w,speed);
  w16(w,bpm);
  for (int i=0; i<256; i++) w8(w,i<(int)order.size()?order[i]:0);

  // channel state carried in song order, to keep Furnace's semantics: volume persists across
  // notes, and the two levels of 81xx/82xx become one volume and one pan.
  std::vector<int> chVol(chans,127), chPanL(chans,255), chPanR(chans,255), chXmVol(chans,-1), chXmPan(chans,128);
  std::map<int,int> volSeen; // xm volume -> furnace volume*1000+pan sum, for the rounding count
  for (size_t p=0; p<patTuples.size(); p++) {
    std::vector<std::vector<Cell>> rows(patLen,std::vector<Cell>(xmChans));
    for (int c=0; c<chans; c++) {
      DivPattern* pat=sub->pat[c].getPattern(patTuples[p][c],false);
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
        if (newNote && cell.ins==0) {
          // XM only resets volume when an instrument is given; Furnace keeps the channel's
        }
        if (d[DIV_PAT_VOL]>=0) chVol[c]=d[DIV_PAT_VOL]&127;
        bool panChanged=false;
        int keyOff=-1;
        int otherFx=-1, otherFxv=0;
        int fxUsed=0;
        for (int k=0; k<fxCols; k++) {
          int fx=d[DIV_PAT_FX(k)], fv=d[DIV_PAT_FXVAL(k)];
          if (fx<0) continue;
          if (fv<0) fv=0;
          switch (fx) {
            case 0x80: chPanL[c]=255-fv; chPanR[c]=fv; panChanged=true; break; // unused by the examples
            case 0x81: chPanL[c]=fv; panChanged=true; break;
            case 0x82: chPanR[c]=fv; panChanged=true; break;
            case 0xfc: keyOff=fv; break;
            case 0x0f: if (otherFx>=0) loss[L_EXTRA_FX]++; otherFx=0x0f; otherFxv=fv; fxUsed++; break;
            case 0x0d: if (otherFx>=0) loss[L_EXTRA_FX]++; otherFx=0x0d; otherFxv=((fv/10)<<4)|(fv%10); fxUsed++; break;
            case 0x0b: if (otherFx>=0) loss[L_EXTRA_FX]++; otherFx=0x0b; otherFxv=fv; fxUsed++; break;
            default: loss[L_EFFECT]++; break;
          }
        }
        // volume and pan from the two levels
        int sum=chPanL[c]+chPanR[c];
        int xmVol=sum?(int)lrint(64.0*chVol[c]*sum/(127.0*510.0)):0;
        int xmPan=sum?MIN(255,(int)lrint(256.0*chPanR[c]/sum)):128;
        int key=chVol[c]*1000+sum;
        auto vs=volSeen.find(xmVol);
        if (vs==volSeen.end()) volSeen[xmVol]=key; else if (vs->second!=key) { loss[L_VOL_QUANT]++; vs->second=key; }
        if (newNote || xmVol!=chXmVol[c]) {
          cell.vol=0x10+xmVol;
          chXmVol[c]=xmVol;
        }
        bool panPending=(panChanged || newNote) && xmPan!=chXmPan[c];
        // effect column: key-off timer first (it decides when the note ends), then the others,
        // then pan
        if (keyOff>=0) {
          cell.fx=0x14; cell.fxv=keyOff;
          if (otherFx>=0) loss[L_EXTRA_FX]++;
        } else if (otherFx>=0) {
          cell.fx=otherFx; cell.fxv=otherFxv;
        } else if (panPending) {
          cell.fx=0x08; cell.fxv=xmPan; chXmPan[c]=xmPan; panPending=false;
        }
        if (panPending) {
          if (cell.vol==0) {
            cell.vol=0xc0|(xmPan>>4); chXmPan[c]=xmPan&0xf0; loss[L_PAN_COARSE]++;
          } else {
            loss[L_PAN_DROPPED]++;
          }
        }
        (void)fxUsed;
      }
    }
    // pack
    SafeWriter* pw=new SafeWriter;
    pw->init();
    for (int r=0; r<patLen; r++) {
      for (int c=0; c<xmChans; c++) {
        Cell& cl=rows[r][c];
        int mask=0x80|(cl.note?1:0)|(cl.ins?2:0)|(cl.vol?4:0)|(cl.fx?8:0)|(cl.fxv?16:0);
        if (mask==0x9f) {
          pw->writeC(cl.note); pw->writeC(cl.ins); pw->writeC(cl.vol); pw->writeC(cl.fx); pw->writeC(cl.fxv);
          continue;
        }
        pw->writeC(mask);
        if (cl.note) pw->writeC(cl.note);
        if (cl.ins) pw->writeC(cl.ins);
        if (cl.vol) pw->writeC(cl.vol);
        if (cl.fx) pw->writeC(cl.fx);
        if (cl.fxv) pw->writeC(cl.fxv);
      }
    }
    w32(w,9);
    w8(w,0);
    w16(w,patLen);
    w16(w,(int)pw->size());
    w->write(pw->getFinalBuf(),pw->size());
    pw->finish();
    delete pw;
    progress[0].amount=0.5f*(p+1)/patTuples.size();
  }

  // instruments
  std::vector<int> sampleUses(e->song.sampleLen,0);
  double tickSamples=44100.0/sub->hz;
  for (int i=0; i<insCount; i++) {
    DivInstrument* ins=e->song.ins[i];
    std::vector<int> smps; // furnace sample index per XM sample slot
    unsigned char keymap[96];
    memset(keymap,0,96);
    for (int n=0; n<96; n++) {
      int fn=n+60;
      int s=ins->amiga.getSample(fn);
      if (ins->amiga.useNoteMap && ins->amiga.getFreq(fn)!=fn) loss[L_NOTEMAP]++;
      if (s<0 || s>=e->song.sampleLen) continue;
      auto it=std::find(smps.begin(),smps.end(),s);
      if (it==smps.end()) {
        if (smps.size()>=16) continue;
        smps.push_back(s);
        it=smps.end()-1;
      }
      keymap[n]=(unsigned char)(it-smps.begin());
    }
    for (int s: smps) sampleUses[s]++;
    w32(w,263);
    wstr(w,ins->name,22);
    w8(w,0);
    w16(w,(int)smps.size());
    if (smps.empty()) continue;
    w32(w,40);
    w->write(keymap,96);
    // volume envelope from the hardware ADSR
    bool isPS1=ins->type==DIV_INS_PS1;
    unsigned short a1=isPS1?ins->ps1.getADSR1():0x000f, a2=isPS1?ins->ps1.getADSR2():0x0000;
    if (isPS1) {
      if (ins->ps1.reverb) loss[L_REVERB]++;
      if (ins->ps1.noise) loss[L_NOISE]++;
      if (ins->ps1.pitchMod) loss[L_PMOD]++;
    }
    ADSRSim sim;
    sim.start(a1,a2);
    int pts[12][2];
    int npts=0;
    int t=0;
    // points at doubling tick intervals capture a fast attack and a long sustain slope alike
    for (int k=0; k<11; k++) {
      pts[npts][0]=t;
      pts[npts][1]=MIN(64,(sim.level*64+0x3fff)/0x7fff);
      npts++;
      int nt=(k==0)?1:t*2;
      sim.run((int)lrint((nt-t)*tickSamples));
      t=nt;
    }
    pts[npts][0]=t; pts[npts][1]=MIN(64,(sim.level*64+0x3fff)/0x7fff); npts++;
    loss[L_ADSR]++;
    for (int k=0; k<12; k++) { w16(w,k<npts?pts[k][0]:0); w16(w,k<npts?pts[k][1]:0); }
    for (int k=0; k<12; k++) { w16(w,0); w16(w,0); }
    w8(w,npts); w8(w,0);
    w8(w,npts-1); w8(w,0); w8(w,0); // sustain at the last point
    w8(w,0); w8(w,0); w8(w,0);
    w8(w,1|2); w8(w,0);
    w8(w,0); w8(w,0); w8(w,0); w8(w,0);
    // release: linear fadeout over the time the hardware release would take from full level
    int rr=a2&31;
    double relSamples=32767.0/((double)(8<<MAX(0,11-rr))/(double)(1<<MAX(0,rr-11)));
    if (a2&0x20) relSamples*=4.0; // exponential: roughly to -60 dB
    double relTicks=MAX(1.0,relSamples/tickSamples);
    int fade=(int)MIN(4095.0,lrint(32768.0/relTicks));
    w16(w,MAX(1,fade));
    loss[L_RELEASE]++;
    w16(w,0);
    for (int k=0; k<20; k++) w8(w,0);
    // sample headers, then data
    std::vector<std::vector<short>> pcm;
    for (int s: smps) {
      DivSample* sm=e->song.sample[s];
      if (sm->depth==DIV_SAMPLE_DEPTH_SPU_ADPCM) loss[L_REENCODE]++;
      if (sm->data16==NULL) sm->render(1<<DIV_SAMPLE_DEPTH_16BIT);
      int len=sm->samples;
      std::vector<short> v(len);
      if (sm->data16) memcpy(v.data(),sm->data16,len*sizeof(short));
      int ls=0, ll=0, type=0x10;
      if (sm->loop && sm->loopEnd>sm->loopStart) {
        ls=sm->loopStart; ll=MIN(len,sm->loopEnd)-ls;
        if (ll>0) type|=1;
      }
      double semis=12.0*log2((double)sm->centerRate/8363.0);
      int rel=(int)floor(semis);
      // linear periods use finetune/2 (64 steps per semitone), so an odd finetune is truncated:
      // round to the nearest 1/64 semitone and write it even
      int ft=2*(int)lrint((semis-rel)*64.0);
      if (ft>127) { ft-=128; rel++; }
      if (rel>127 || rel<-128) loss[L_NOTE_RANGE]++;
      w32(w,len*2); w32(w,ls*2); w32(w,ll*2);
      w8(w,64); w8(w,(signed char)ft); w8(w,type); w8(w,128); w8(w,(signed char)CLAMP(rel,-128,127)); w8(w,0);
      wstr(w,sm->name,22);
      pcm.push_back(v);
    }
    for (auto& v: pcm) {
      short prev=0;
      for (short x: v) { w16(w,(short)(x-prev)); prev=x; }
    }
    progress[0].amount=0.5f+0.5f*(i+1)/insCount;
  }
  for (int s=0; s<e->song.sampleLen; s++) if (sampleUses[s]>1) loss[L_SHARED_SAMPLE]++;

  logAppendf("%d channels, %d orders, %d patterns, %d instruments, speed %d, BPM %d",xmChans,(int)order.size(),(int)patTuples.size(),insCount,speed,bpm);
  int total=0;
  for (int i=0; i<L_MAX; i++) {
    if (!loss[i]) continue;
    logAppendf("LOSS %s: %d",lossNames[i],loss[i]);
    total++;
  }
  if (!total) logAppend("no losses");
  progress[0].amount=1.0f;
  logAppend("finished!");
  output.push_back(DivROMExportOutput("out.xm",w));
  running=false;
}

bool DivExportXM::go(DivEngine* eng) {
  progress[0].name="Export";
  progress[0].amount=0.0f;
  e=eng;
  running=true;
  failed=false;
  mustAbort=false;
  exportThread=new std::thread(&DivExportXM::run,this);
  return true;
}

bool DivExportXM::isRunning() {
  return running;
}

bool DivExportXM::hasFailed() {
  return failed;
}

void DivExportXM::abort() {
  mustAbort=true;
  wait();
}

void DivExportXM::wait() {
  if (exportThread!=NULL) {
    exportThread->join();
    delete exportThread;
    exportThread=NULL;
  }
}

DivROMExportProgress DivExportXM::getProgress(int index) {
  return progress[0];
}
