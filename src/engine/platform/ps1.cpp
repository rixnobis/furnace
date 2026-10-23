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

#include "ps1.h"
#include "../engine.h"
#include <math.h>

// the SPU pitch register is a 4.12 step at 44.1 kHz: 0x1000 plays a sample at 44100 Hz.
#define CHIP_FREQBASE 4096
// the hardware clips any step above 0x4000 to 0x4000 (176.4 kHz).
#define PS1_MAX_PITCH 0x4000

// SPU RAM 0x00000-0x00fff holds the CD and voice 1/3 capture buffers, written by the
// hardware all the time. samples go after them.
#define PS1_SAMPLE_BASE 0x1000

// voice registers: 0x1F801C00 + voice*16
#define PS1_VOL_L 0x0
#define PS1_VOL_R 0x2
#define PS1_PITCH 0x4
#define PS1_START 0x6
#define PS1_ADSR1 0x8
#define PS1_ADSR2 0xa
#define PS1_REPEAT 0xe
// global registers
#define PS1_KON_LO 0xd88
#define PS1_KON_HI 0xd8a
#define PS1_KOFF_LO 0xd8c
#define PS1_KOFF_HI 0xd8e
#define PS1_PMON_LO 0xd90
#define PS1_NON_LO 0xd94
#define PS1_EON_LO 0xd98
// fixed (direct) volume mode: bit 15 must stay clear (it selects sweep mode).
#define PS1_VOL_MAX 0x3fff

void DivPlatformPS1::spuWrite(unsigned int addr, unsigned short val) {
  if (skipRegisterWrites) return;
  addr&=0xfff;
  if (addr>=0xc00 && addr<0xe00) regPool[(addr-0xc00)>>1]=val;
  spu->writeReg(addr,val);
  if (dumpWrites) addWrite(addr,val);
}

void DivPlatformPS1::voiceWrite(int ch, unsigned int reg, unsigned short val) {
  spuWrite(0xc00+(ch<<4)+reg,val);
}

// update bit ch of a 24-bit voice mask register pair (lo: voices 0-15, lo+2: voices 16-23).
void DivPlatformPS1::writeMask(unsigned int regLo, unsigned int& mask, int ch, bool on, bool force) {
  unsigned int newMask=on?(mask|(1U<<ch)):(mask&~(1U<<ch));
  if (newMask==mask && !force) return;
  mask=newMask;
  if (ch<16) {
    spuWrite(regLo,mask&0xffff);
  } else {
    spuWrite(regLo+2,(mask>>16)&0xff);
  }
}

void DivPlatformPS1::acquire(short** buf, size_t len) {
  for (int i=0; i<24; i++) {
    oscBuf[i]->begin(len);
  }
  // TODO: per-voice oscilloscope (the core does not expose per-voice output).
  spu->render(buf[0],buf[1],len);
  for (int i=0; i<24; i++) {
    oscBuf[i]->end(len);
  }
}

void DivPlatformPS1::tick(bool sysTick) {
  for (int i=0; i<24; i++) {
    chan[i].std.next();
    if (chan[i].std.vol.had) {
      chan[i].outVol=VOL_SCALE_LINEAR(chan[i].vol&127,MIN(127,chan[i].std.vol.val),127);
      chan[i].volumeChanged=true;
    }
    if (NEW_ARP_STRAT) {
      chan[i].handleArp();
    } else if (chan[i].std.arp.had && !chan[i].rawFreq) {
      if (!chan[i].inPorta) {
        chan[i].baseFreq=chan[i].calcBaseFreq(parent->calcArp(chan[i].note,chan[i].std.arp.val));
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.pitch.had) {
      if (chan[i].std.pitch.mode) {
        chan[i].pitch2+=chan[i].std.pitch.val;
        CLAMP_VAR(chan[i].pitch2,-32768,32767);
      } else {
        chan[i].pitch2=chan[i].std.pitch.val;
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].volumeChanged) {
      int v=isMuted[i]?0:(chan[i].outVol*PS1_VOL_MAX)/127;
      voiceWrite(i,PS1_VOL_L,(unsigned short)((v*chan[i].panL)/255));
      voiceWrite(i,PS1_VOL_R,(unsigned short)((v*chan[i].panR)/255));
      chan[i].volumeChanged=false;
    }
    if (chan[i].freqChanged || chan[i].keyOn || chan[i].keyOff) {
      chan[i].freq=chan[i].calcFreq();
      if (chan[i].freq<0) chan[i].freq=0;
      if (chan[i].freq>PS1_MAX_PITCH) chan[i].freq=PS1_MAX_PITCH;
      voiceWrite(i,PS1_PITCH,chan[i].freq);
      if (chan[i].keyOn) {
        int s=chan[i].sample;
        if (s>=0 && s<parent->song.sampleLen && sampleLoaded[s]) {
          voiceWrite(i,PS1_START,sampleOff[s]>>3);
          voiceWrite(i,PS1_ADSR1,chan[i].adsr1);
          voiceWrite(i,PS1_ADSR2,chan[i].adsr2);
          // voice 0 cannot be pitch-modulated (there is no voice before it).
          writeMask(PS1_PMON_LO,pmonMask,i,chan[i].pitchMod && i>0);
          writeMask(PS1_NON_LO,nonMask,i,chan[i].noise);
          writeMask(PS1_EON_LO,eonMask,i,chan[i].reverb);
          spuWrite((i<16)?PS1_KON_LO:PS1_KON_HI,1<<(i&15));
        }
      }
      if (chan[i].keyOff) {
        spuWrite((i<16)?PS1_KOFF_LO:PS1_KOFF_HI,1<<(i&15));
      }
      chan[i].keyOn=false;
      chan[i].keyOff=false;
      chan[i].freqChanged=false;
    }
  }
}

int DivPlatformPS1::dispatch(DivCommand c) {
  switch (c.cmd) {
    case DIV_CMD_NOTE_ON: {
      DivInstrument* ins=parent->getIns(chan[c.chan].ins,DIV_INS_PS1);
      // the envelope comes from a PS1 instrument; any other type (e.g. Amiga) gets the defaults.
      DivInstrumentPS1 env;
      if (ins->type==DIV_INS_PS1) env=ins->ps1;
      chan[c.chan].adsr1=env.getADSR1();
      chan[c.chan].adsr2=env.getADSR2();
      chan[c.chan].reverb=env.reverb;
      chan[c.chan].noise=env.noise;
      chan[c.chan].pitchMod=env.pitchMod;
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].sample=ins->amiga.getSample(c.value);
        chan[c.chan].pitchTable=samplePitchTable.get(chan[c.chan].sample);
        chan[c.chan].sampleNote=c.value;
        c.value=ins->amiga.getFreq(c.value);
        chan[c.chan].sampleNoteDelta=c.value-chan[c.chan].sampleNote;
        chan[c.chan].baseFreq=chan[c.chan].calcBaseFreq(c.value);
        chan[c.chan].freqChanged=true;
        chan[c.chan].note=c.value;
      }
      if (chan[c.chan].sample<0 || chan[c.chan].sample>=parent->song.sampleLen) {
        chan[c.chan].sample=-1;
      }
      chan[c.chan].active=true;
      chan[c.chan].keyOn=true;
      chan[c.chan].macroInit(ins);
      if (!parent->song.compatFlags.brokenOutVol && !chan[c.chan].std.vol.will) {
        chan[c.chan].outVol=chan[c.chan].vol;
      }
      chan[c.chan].volumeChanged=true;
      break;
    }
    case DIV_CMD_NOTE_OFF:
    case DIV_CMD_NOTE_OFF_ENV:
    case DIV_CMD_ENV_RELEASE:
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      if (c.cmd==DIV_CMD_NOTE_OFF) {
        chan[c.chan].macroInit(NULL);
      } else {
        chan[c.chan].std.release();
      }
      break;
    case DIV_CMD_INSTRUMENT:
      if (chan[c.chan].ins!=c.value || c.value2==1) {
        chan[c.chan].ins=c.value;
      }
      break;
    case DIV_CMD_VOLUME:
      if (chan[c.chan].vol!=c.value) {
        chan[c.chan].vol=c.value;
        if (!chan[c.chan].std.vol.has) {
          chan[c.chan].outVol=c.value;
          chan[c.chan].volumeChanged=true;
        }
      }
      break;
    case DIV_CMD_GET_VOLUME:
      if (chan[c.chan].std.vol.has) {
        return chan[c.chan].vol;
      }
      return chan[c.chan].outVol;
      break;
    case DIV_CMD_PANNING:
      chan[c.chan].panL=c.value;
      chan[c.chan].panR=c.value2;
      chan[c.chan].volumeChanged=true;
      break;
    case DIV_CMD_PITCH:
      chan[c.chan].pitch=c.value;
      chan[c.chan].freqChanged=true;
      break;
    case DIV_CMD_GET_VOLMAX:
      return 127;
      break;
    case DIV_CMD_MACRO_OFF:
      chan[c.chan].std.mask(c.value,true);
      break;
    case DIV_CMD_MACRO_ON:
      chan[c.chan].std.mask(c.value,false);
      break;
    case DIV_CMD_MACRO_RESTART:
      chan[c.chan].std.restart(c.value);
      break;
    default:
      // everything else is not implemented yet.
      break;
  }
  return 1;
}

void DivPlatformPS1::muteChannel(int ch, bool mute) {
  isMuted[ch]=mute;
  chan[ch].volumeChanged=true;
}

void DivPlatformPS1::forceIns() {
  for (int i=0; i<24; i++) {
    chan[i].insChanged=true;
    chan[i].volumeChanged=true;
    chan[i].freqChanged=true;
  }
}

SharedChannel* DivPlatformPS1::getChanState(int ch) {
  return &chan[ch];
}

DivMacroInt* DivPlatformPS1::getChanMacroInt(int ch) {
  return &chan[ch].std;
}

DivDispatchOscBuffer* DivPlatformPS1::getOscBuffer(int ch) {
  return oscBuf[ch];
}

unsigned char* DivPlatformPS1::getRegisterPool() {
  return (unsigned char*)regPool;
}

int DivPlatformPS1::getRegisterPoolSize() {
  return 256;
}

int DivPlatformPS1::getRegisterPoolDepth() {
  return 16;
}

void DivPlatformPS1::reset() {
  memset(regPool,0,sizeof(regPool));
  spu->reset();
  pmonMask=0;
  nonMask=0;
  eonMask=0;
  for (unsigned int r: {PS1_PMON_LO,PS1_NON_LO,PS1_EON_LO}) {
    spuWrite(r,0);
    spuWrite(r+2,0);
  }
  for (int i=0; i<24; i++) {
    chan[i]=DivPlatformPS1::Channel(parent->song.compatFlags.linearPitch);
    chan[i].pitchTable=samplePitchTable.get(-1);
    chan[i].std.setEngine(parent);
  }
}

int DivPlatformPS1::getOutputCount() {
  return 2;
}

void DivPlatformPS1::notifyInsChange(int ins) {
  for (int i=0; i<24; i++) {
    if (chan[i].ins==ins) {
      chan[i].insChanged=true;
    }
  }
}

void DivPlatformPS1::notifyInsDeletion(void* ins) {
  for (int i=0; i<24; i++) {
    chan[i].std.notifyInsDeletion((DivInstrument*)ins);
  }
}

void DivPlatformPS1::notifyPitchTable(int sample) {
  samplePitchTable.update<Channel>(chan,24,parent->song.tuning,chipClock,CHIP_FREQBASE,PS1_MAX_PITCH,false,parent->song.compatFlags.linearPitch,sample);
}

unsigned int DivPlatformPS1::getMaxFreq(int ch) {
  return PS1_MAX_PITCH;
}

void DivPlatformPS1::setFlags(const DivConfig& flags) {
  // the SPU mixes at a fixed 44.1 kHz; pitch 0x1000 is 44100 Hz.
  chipClock=PS1SPUBridge::RATE;
  rate=PS1SPUBridge::RATE;
  for (int i=0; i<24; i++) {
    oscBuf[i]->setRate(rate);
  }
  notifyPitchTable();
}

void DivPlatformPS1::poke(unsigned int addr, unsigned short val) {
  spuWrite(addr,val);
}

void DivPlatformPS1::poke(std::vector<DivRegWrite>& wlist) {
  for (DivRegWrite& i: wlist) spuWrite(i.addr,i.val);
}

const void* DivPlatformPS1::getSampleMem(int index) {
  return index==0?sampleMem:NULL;
}

size_t DivPlatformPS1::getSampleMemCapacity(int index) {
  return index==0?PS1SPUBridge::RAM_SIZE:0;
}

size_t DivPlatformPS1::getSampleMemUsage(int index) {
  return index==0?sampleMemLen:0;
}

bool DivPlatformPS1::isSampleLoaded(int index, int sample) {
  if (index!=0) return false;
  if (sample<0 || sample>32767) return false;
  return sampleLoaded[sample];
}

const DivMemoryComposition* DivPlatformPS1::getMemCompo(int index) {
  if (index!=0) return NULL;
  return &memCompo;
}

void DivPlatformPS1::uploadSamples() {
  if (sampleMemLen>PS1_SAMPLE_BASE) {
    spu->writeRAM(PS1_SAMPLE_BASE,sampleMem+PS1_SAMPLE_BASE,sampleMemLen-PS1_SAMPLE_BASE);
  }
}

void DivPlatformPS1::renderSamples(int sysID) {
  memset(sampleMem,0,PS1SPUBridge::RAM_SIZE);
  memset(sampleOff,0,32768*sizeof(unsigned int));
  memset(sampleLoaded,0,32768*sizeof(bool));

  memCompo=DivMemoryComposition();
  memCompo.name="SPU RAM";
  memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_RESERVED,"Capture buffers",-1,0,PS1_SAMPLE_BASE));

  size_t memPos=PS1_SAMPLE_BASE;
  for (int i=0; i<parent->song.sampleLen && i<32768; i++) {
    DivSample* s=parent->song.sample[i];
    if (s->data16==NULL || s->samples==0) continue;
    if (!s->renderOn[0][sysID]) continue;
    size_t need=PS1SPUBridge::encodedSize(s->samples);
    if (memPos+need>PS1SPUBridge::RAM_SIZE) {
      logW("out of PS1 SPU memory for sample %d!",i);
      break;
    }
    int loopStart=s->isLoopable()?s->loopStart:-1;
    size_t len=PS1SPUBridge::encodeADPCM(s->data16,s->samples,loopStart,s->loopEnd,sampleMem+memPos,PS1SPUBridge::RAM_SIZE-memPos);
    if (len==0) continue;
    sampleOff[i]=memPos;
    sampleLoaded[i]=true;
    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_SAMPLE,"Sample",i,memPos,memPos+len));
    memPos+=len;
  }
  sampleMemLen=memPos;
  memCompo.used=sampleMemLen;
  memCompo.capacity=PS1SPUBridge::RAM_SIZE;
  uploadSamples();
}

int DivPlatformPS1::init(DivEngine* p, int channels, int sugRate, const DivConfig& flags) {
  parent=p;
  samplePitchTable.init(parent);
  dumpWrites=false;
  skipRegisterWrites=false;
  for (int i=0; i<24; i++) {
    isMuted[i]=false;
    oscBuf[i]=new DivDispatchOscBuffer;
  }
  sampleMem=new unsigned char[PS1SPUBridge::RAM_SIZE];
  memset(sampleMem,0,PS1SPUBridge::RAM_SIZE);
  sampleMemLen=0;
  spu=new PS1SPUBridge;
  setFlags(flags);
  reset();
  return 24;
}

void DivPlatformPS1::quit() {
  delete spu;
  spu=NULL;
  delete[] sampleMem;
  sampleMem=NULL;
  for (int i=0; i<24; i++) {
    delete oscBuf[i];
  }
}

DivPlatformPS1::DivPlatformPS1():
  DivDispatch(),
  spu(NULL),
  sampleMem(NULL),
  sampleMemLen(0),
  pmonMask(0),
  nonMask(0),
  eonMask(0) {
  sampleOff=new unsigned int[32768];
  sampleLoaded=new bool[32768];
  memset(sampleOff,0,32768*sizeof(unsigned int));
  memset(sampleLoaded,0,32768*sizeof(bool));
  memset(regPool,0,sizeof(regPool));
}

DivPlatformPS1::~DivPlatformPS1() {
  delete[] sampleOff;
  delete[] sampleLoaded;
  samplePitchTable.destroy<Channel>(chan,24);
}
