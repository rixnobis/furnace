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

#include "snes.h"
#include "../engine.h"
#include "../../ta-log.h"
#include "furIcons.h"
#include <math.h>

#define CHIP_FREQBASE 131072

#define rWrite(a,v) if (!skipRegisterWrites) {writes.push(QueuedWrite(a,v)); if (dumpWrites) {addWrite(a,v);} }
#define chWrite(c,a,v) {rWrite((a)+(c)*16,v)}
#define rWriteDelay(a,v,d) if (!skipRegisterWrites) {writes.push(QueuedWrite(a,v,d)); if (dumpWrites) {addWrite(a,v);} }
#define chWriteDelay(c,a,v,d) {rWrite((a)+(c)*16,v,d)}
#define sampleTableAddr(c) (sampleTableBase+(c)*4)
#define waveTableAddr(c) (sampleTableBase+8*4+(c)*9*16)

// PS1 reverb presets from psx-spx (32 register values each)
// order: dAPF1,dAPF2,vIIR,vCOMB1,vCOMB2,vCOMB3,vCOMB4,vWALL,
//        vAPF1,vAPF2,mLSAME,mRSAME,mLCOMB1,mRCOMB1,mLCOMB2,mRCOMB2,
//        dLSAME,dRSAME,mLDIFF,mRDIFF,mLCOMB3,mRCOMB3,mLCOMB4,mRCOMB4,
//        dLDIFF,dRDIFF,mLAPF1,mRAPF1,mLAPF2,mRAPF2,vLIN,vRIN
static const short ps1ReverbPresets[][32]={
  // 0: Off (size=0x10)
  {0,0,0,0,0,0,0,0,0,0,1,1,1,1,1,1,0,0,1,1,1,1,1,1,0,0,1,1,1,1,0,0},
  // 1: Room (size=0x26C0)
  {0x007D,0x005B,0x6D80,0x54B8,(short)0xBED0,0x0000,0x0000,(short)0xBA80,
   0x5800,0x5300,0x04D6,0x0333,0x03F0,0x0227,0x0374,0x01EF,
   0x0334,0x01B5,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
   0x0000,0x0000,0x01B4,0x0136,0x00B8,0x005C,(short)0x8000,(short)0x8000},
  // 2: Studio Small (size=0x1F40)
  {0x0033,0x0025,0x70F0,0x4FA8,(short)0xBCE0,0x4410,(short)0xC0F0,(short)0x9C00,
   0x5280,0x4EC0,0x03E4,0x031B,0x03A4,0x02AF,0x0372,0x0266,
   0x031C,0x025D,0x025C,0x018E,0x022F,0x0135,0x01D2,0x00B7,
   0x018F,0x00B5,0x00B4,0x0080,0x004C,0x0026,(short)0x8000,(short)0x8000},
  // 3: Studio Medium (size=0x4840)
  {0x00B1,0x007F,0x70F0,0x4FA8,(short)0xBCE0,0x4510,(short)0xBEF0,(short)0xB4C0,
   0x5280,0x4EC0,0x0904,0x076B,0x0824,0x065F,0x07A2,0x0616,
   0x076C,0x05ED,0x05EC,0x042E,0x050F,0x0305,0x0462,0x02B7,
   0x042F,0x0265,0x0264,0x01B2,0x0100,0x0080,(short)0x8000,(short)0x8000},
  // 4: Studio Large (size=0x6FE0)
  {0x00E3,0x00A9,0x6F60,0x4FA8,(short)0xBCE0,0x4510,(short)0xBEF0,(short)0xA680,
   0x5680,0x52C0,0x0DFB,0x0B58,0x0D09,0x0A3C,0x0BD9,0x0973,
   0x0B59,0x08DA,0x08D9,0x05E9,0x07EC,0x04B0,0x06EF,0x03D2,
   0x05EA,0x031D,0x031C,0x0238,0x0154,0x00AA,(short)0x8000,(short)0x8000},
  // 5: Hall (size=0xADE0)
  {0x01A5,0x0139,0x6000,0x5000,0x4C00,(short)0xB800,(short)0xBC00,(short)0xC000,
   0x6000,0x5C00,0x15BA,0x11BB,0x14C2,0x10BD,0x11BC,0x0DC1,
   0x11C0,0x0DC3,0x0DC0,0x09C1,0x0BC4,0x07C1,0x0A00,0x06CD,
   0x09C2,0x05C1,0x05C0,0x041A,0x0274,0x013A,(short)0x8000,(short)0x8000},
  // 6: Half Echo (size=0x3C00)
  {0x0017,0x0013,0x70F0,0x4FA8,(short)0xBCE0,0x4510,(short)0xBEF0,(short)0x8500,
   0x5F80,0x54C0,0x0371,0x02AF,0x02E5,0x01DF,0x02B0,0x01D7,
   0x0358,0x026A,0x01D6,0x011E,0x012D,0x00B1,0x011F,0x0059,
   0x01A0,0x00E3,0x0058,0x0040,0x0028,0x0014,(short)0x8000,(short)0x8000},
  // 7: Space Echo (size=0xF6C0)
  {0x033D,0x0231,0x7E00,0x5000,(short)0xB400,(short)0xB000,0x4C00,(short)0xB000,
   0x6000,0x5400,0x1ED6,0x1A31,0x1D14,0x183B,0x1BC2,0x16B2,
   0x1A32,0x15EF,0x15EE,0x1055,0x1334,0x0F2D,0x11F6,0x0C5D,
   0x1056,0x0AE1,0x0AE0,0x07A2,0x0464,0x0232,(short)0x8000,(short)0x8000},
  // 8: Chaos Echo (size=0x18040)
  {0x0001,0x0001,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,(short)0x8100,
   0x0000,0x0000,0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,
   0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
   0x0000,0x0000,0x1004,0x1002,0x0004,0x0002,(short)0x8000,(short)0x8000},
  // 9: Delay (size=0x18040)
  {0x0001,0x0001,0x7FFF,0x7FFF,0x0000,0x0000,0x0000,0x0000,
   0x0000,0x0000,0x1FFF,0x0FFF,0x1005,0x0005,0x0000,0x0000,
   0x1005,0x0005,0x0000,0x0000,0x0000,0x0000,0x0000,0x0000,
   0x0000,0x0000,0x1004,0x1002,0x0004,0x0002,(short)0x8000,(short)0x8000},
};

// reverb buffer sizes in bytes for each preset
static const unsigned int ps1ReverbSizes[]={
  0x10, 0x26C0, 0x1F40, 0x4840, 0x6FE0, 0xADE0, 0x3C00, 0xF6C0, 0x18040, 0x18040
};

// Reverb preset names live in src/gui/sysConf.cpp as inline strings used by the
// preset combo box. The array used to live here too but was never referenced.

#define PS1_REVERB_PRESET_COUNT 10

// PS1 SPU register write - cached, only emits when value changes
// addresses are offsets from SPU base (0x1F801C00)
#define ps1Write(a,v) { \
  unsigned short _a=(unsigned short)(a); \
  unsigned short _v=(unsigned short)(v); \
  if (!skipRegisterWrites && dumpWrites && (_a<0x200) && (ps1RegCache[_a]!=_v)) { \
    ps1RegCache[_a]=_v; \
    addWrite(_a,_v); \
  } \
}
#define ps1WriteForce(a,v) if (!skipRegisterWrites && dumpWrites) {addWrite(a,v);}
#define ps1ChWrite(c,a,v) {ps1Write((a)+(c)*0x10,v)}
#define ps1ChWriteForce(c,a,v) {ps1WriteForce((a)+(c)*0x10,v)}

// PS1 SPU register offsets (per-voice, offset from voice base = ch*0x10)
#define PS1_REG_VOL_L      0x00
#define PS1_REG_VOL_R      0x02
#define PS1_REG_PITCH      0x04
#define PS1_REG_START_ADDR 0x06
#define PS1_REG_ADSR_LO    0x08
#define PS1_REG_ADSR_HI    0x0A
#define PS1_REG_ADSR_VOL   0x0C
#define PS1_REG_LOOP_ADDR  0x0E

// PS1 SPU global register offsets (from 0x1F801C00)
#define PS1_REG_MAIN_VOL_L 0x180
#define PS1_REG_MAIN_VOL_R 0x182
#define PS1_REG_KEY_ON_LO  0x188
#define PS1_REG_KEY_ON_HI  0x18A
#define PS1_REG_KEY_OFF_LO 0x18C
#define PS1_REG_KEY_OFF_HI 0x18E
#define PS1_REG_PMOD_LO    0x190
#define PS1_REG_PMOD_HI    0x192
#define PS1_REG_NOISE_LO   0x194
#define PS1_REG_NOISE_HI   0x196
#define PS1_REG_REVERB_LO  0x198
#define PS1_REG_REVERB_HI  0x19A
#define PS1_REG_NOISE_FREQ 0x19C

const char* regCheatSheetSNESDSP[]={
  "VxVOLL", "x0",
  "VxVOLR", "x1",
  "VxPITCHL", "x2",
  "VxPITCHH", "x3",
  "VxSRCN", "x4",
  "VxADSR1", "x5",
  "VxADSR2", "x6",
  "VxGAIN", "x7",
  "VxENVX", "x8",
  "VxOUTX", "x9",
  "FIRx", "xF",

  "MVOLL", "0C",
  "MVOLR", "1C",
  "EVOLL", "2C",
  "EVOLR", "3C",
  "KON", "4C",
  "KOFF", "5C",
  "FLG", "6C",
  "ENDX", "7C",

  "EFB", "0D",
  "PMON", "2D",
  "NON", "3D",
  "EON", "4D",
  "DIR", "5D",
  "ESA", "6D",
  "EDL", "7D",
  NULL
};

const char** DivPlatformSNES::getRegisterSheet() {
  return regCheatSheetSNESDSP;
}

void DivPlatformSNES::acquire(short** buf, size_t len) {
  short out[2];
  short chOut[SNES_PSX_MAX_CHAN*2];
  for (int i=0; i<chanCount; i++) {
    oscBuf[i]->begin(len);
  }
  for (size_t h=0; h<len; h++) {
    if (!ps1Mode) {
      if (--delay<=0) {
        delay=0;
        if (!writes.empty()) {
          QueuedWrite w=writes.front();
          dsp.write(w.addr,w.val);
          regPool[w.addr&0x7f]=w.val;
          writes.pop();
          delay=w.delay;
        }
      }
    }
    dsp.set_output(out,1);
    if (ps1Mode) {
      dsp.runPS1(1);
    } else {
      dsp.run(32);
    }
    dsp.get_voice_outputs(chOut);
    buf[0][h]=out[0];
    buf[1][h]=out[1];
    for (int i=0; i<chanCount; i++) {
      int next=(3*(chOut[i*2]+chOut[i*2+1]))>>2;
      if (next<-32768) next=-32768;
      if (next>32767) next=32767;
      next=(next*254)/MAX(1,globalVolL+globalVolR);
      if (next<-32768) next=-32768;
      if (next>32767) next=32767;
      oscBuf[i]->putSample(h,next>>1);
    }
  }
  for (int i=0; i<chanCount; i++) {
    oscBuf[i]->end(len);
  }
}

void DivPlatformSNES::tick(bool sysTick) {
  // KON/KOFF can't be written several times per one sample
  // so they have to be accumulated
  unsigned int kon=0;
  unsigned int koff=0;
  for (int i=0; i<chanCount; i++) {
    chan[i].std.next();
    if (chan[i].std.vol.had) {
      chan[i].outVol=VOL_SCALE_LINEAR(chan[i].vol&127,MIN(127,chan[i].std.vol.val),127);
    }
    if (NEW_ARP_STRAT) {
      chan[i].handleArp();
    } else if (chan[i].std.arp.had) {
      if (!chan[i].inPorta) {
        chan[i].baseFreq=NOTE_FREQUENCY(parent->calcArp(chan[i].note,chan[i].std.arp.val));
      }
      chan[i].freqChanged=true;
    }
    if (chan[i].std.duty.had) {
      noiseFreq=chan[i].std.duty.val;
      writeControl=true;
    }
    if (!ps1Mode && chan[i].useWave && chan[i].std.wave.had) {
      if (chan[i].wave!=chan[i].std.wave.val || chan[i].ws.activeChanged()) {
        chan[i].wave=chan[i].std.wave.val;
        chan[i].ws.changeWave1(chan[i].wave);
      }
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
    if (chan[i].std.panL.had) {
      chan[i].panL=chan[i].std.panL.val&0x7f;
    }
    if (chan[i].std.panR.had) {
      chan[i].panR=chan[i].std.panR.val&0x7f;
    }
    bool hasInverted=false;
    if (chan[i].std.ex1.had) {
      if (chan[i].invertL!=(bool)(chan[i].std.ex1.val&16)) {
        chan[i].invertL=chan[i].std.ex1.val&16;
        hasInverted=true;
      }
      if (chan[i].invertR!=(bool)(chan[i].std.ex1.val&8)) {
        chan[i].invertR=chan[i].std.ex1.val&8;
        hasInverted=true;
      }
      if (chan[i].pitchMod!=(bool)(chan[i].std.ex1.val&4)) {
        chan[i].pitchMod=chan[i].std.ex1.val&4;
        writePitchMod=true;
      }
      if (chan[i].echo!=(bool)(chan[i].std.ex1.val&2)) {
        chan[i].echo=chan[i].std.ex1.val&2;
        writeEcho=true;
      }
      if (chan[i].noise!=(bool)(chan[i].std.ex1.val&1)) {
        chan[i].noise=chan[i].std.ex1.val&1;
        writeNoise=true;
      }
    }
    if (chan[i].std.vol.had || chan[i].std.panL.had || chan[i].std.panR.had || hasInverted) {
      chan[i].shallWriteVol=true;
    }
    if (!ps1Mode && chan[i].std.ex2.had) {
      if (chan[i].std.ex2.val&0x80) {
        switch (chan[i].std.ex2.val&0x60) {
          case 0x00:
            chan[i].state.gainMode=DivInstrumentSNES::GAIN_MODE_DEC_LINEAR;
            break;
          case 0x20:
            chan[i].state.gainMode=DivInstrumentSNES::GAIN_MODE_DEC_LOG;
            break;
          case 0x40:
            chan[i].state.gainMode=DivInstrumentSNES::GAIN_MODE_INC_LINEAR;
            break;
          case 0x60:
            chan[i].state.gainMode=DivInstrumentSNES::GAIN_MODE_INC_INVLOG;
            break;
        }
        chan[i].state.gain=chan[i].std.ex2.val&31;
      } else {
        chan[i].state.gainMode=DivInstrumentSNES::GAIN_MODE_DIRECT;
        chan[i].state.gain=chan[i].std.ex2.val&127;
      }
      chan[i].shallWriteEnv=true;
    }
    if (chan[i].setPos) {
      // force keyon
      chan[i].keyOn=true;
      chan[i].setPos=false;
    } else {
      chan[i].audPos=0;
    }
    if (!ps1Mode && chan[i].useWave && chan[i].active) {
      if (chan[i].ws.tick()) {
        updateWave(i);
      }
    }
  }
  for (int i=0; i<chanCount; i++) {
    // TODO: if wavetable length is higher than 32, we lose precision!
    if (chan[i].freqChanged || chan[i].keyOn || chan[i].keyOff) {
      DivSample* s=parent->getSample(chan[i].sample);
      double off=(s->centerRate>=1)?((double)s->centerRate/parent->getCenterRate()):1.0;
      if (!ps1Mode && chan[i].useWave) off=(double)chan[i].wtLen/32.0;
      chan[i].freq=(unsigned int)(off*parent->calcFreq(chan[i].baseFreq,chan[i].pitch,chan[i].fixedArp?chan[i].baseNoteOverride:chan[i].arpOff,chan[i].fixedArp,false,2,chan[i].pitch2,chipClock,CHIP_FREQBASE));
      if (chan[i].freq>16383) chan[i].freq=16383;
      if (chan[i].keyOn) {
        if (ps1Mode) {
          // PS1 mode: set voice sample address directly in DSP
          if (chan[i].sample>=0 && chan[i].sample<parent->song.sampleLen) {
            SPC_DSP::voice_t* v=const_cast<SPC_DSP::voice_t*>(dsp.get_voice(i));
            v->brr_addr=sampleOff[chan[i].sample];
            if (chan[i].audPos>0) {
              v->brr_addr+=((chan[i].audPos/28)*16);
            }
            v->brr_offset=0;
            v->kon_delay=5;
            v->env_mode=SPC_DSP::env_attack;
            v->env=0;
            v->ps1_env_frac=0;
            // emit SPU register writes for export
            if (dumpWrites && chan[i].insChanged) {
              // macro invocation covers START_ADDR + ADSR, so emit invocation only
              addWrite(0xF000|(chan[i].ins&0x0FFF),i);
              // pre-populate cache to suppress redundant raw writes from writeEnv()
              // the macro already covers START_ADDR + ADSR
              unsigned int startAddr8=sampleOff[chan[i].sample]/8;
              unsigned short regBase=i*0x10;
              ps1RegCache[regBase+PS1_REG_START_ADDR]=startAddr8&0xffff;
              // compute ADSR values that writeEnv() will try to write
              const DivInstrumentPS1& p=chan[i].ps1State;
              unsigned short adsrLo=(p.s&0xf)|((p.d&0xf)<<4)|((p.a&0x7f)<<8)|((p.aExp?1:0)<<15);
              unsigned short adsrHi=(p.r&0x1f)|((p.rExp?1:0)<<5)|((p.sr&0x7f)<<6)|((p.sDir?1:0)<<14)|((p.sExp?1:0)<<15);
              ps1RegCache[regBase+PS1_REG_ADSR_LO]=adsrLo;
              ps1RegCache[regBase+PS1_REG_ADSR_HI]=adsrHi;
            } else {
              // no macro - emit raw start address
              unsigned int startAddr8=sampleOff[chan[i].sample]/8;
              ps1ChWrite(i,PS1_REG_START_ADDR,startAddr8&0xffff);
            }
            // loop address is set automatically by ADPCM block flags in sample data
          }
          kon|=(1<<i);
          koff|=(1<<i);
        } else {
          // SNES mode: write sample directory table
          unsigned int start, end, loop;
          unsigned short tabAddr=sampleTableAddr(i);
          if (chan[i].useWave) {
            start=waveTableAddr(i);
            loop=start;
          } else if (chan[i].sample>=0 && chan[i].sample<parent->song.sampleLen) {
            start=sampleOff[chan[i].sample];
            end=MIN(start+MAX(s->lengthBRR+((s->loop && s->depth!=DIV_SAMPLE_DEPTH_BRR)?9:0),1),getSampleMemCapacity());
            loop=MAX(start,end-1);
            if (chan[i].audPos>0) {
              start=start+MIN(chan[i].audPos/16*9,end-start);
            }
            if (s->isLoopable()) {
              loop=((s->depth!=DIV_SAMPLE_DEPTH_BRR)?9:0)+start+((s->loopStart/16)*9);
            }
          } else {
            start=0;
            end=0;
            loop=0;
          }
          sampleMem[tabAddr+0]=start&0xff;
          sampleMem[tabAddr+1]=start>>8;
          sampleMem[tabAddr+2]=loop&0xff;
          sampleMem[tabAddr+3]=loop>>8;
          kon|=(1<<i);
          koff|=(1<<i);
        }
        chan[i].keyOn=false;
      }
      if (chan[i].keyOff) {
        if (ps1Mode) {
          koff|=(1<<i);
          // transition the DSP voice into release for in-emulator playback
          SPC_DSP::voice_t* v=const_cast<SPC_DSP::voice_t*>(dsp.get_voice(i));
          v->env_mode=SPC_DSP::env_release;
          v->ps1_env_frac=0;
        } else if (!chan[i].state.sus) {
          koff|=(1<<i);
        }
        chan[i].keyOff=false;
      }
      if (chan[i].freqChanged) {
        if (ps1Mode) {
          // PS1 SPU pitch: 4.12 fixed-point, 0x1000 = 44100Hz
          ps1ChWrite(i,PS1_REG_PITCH,chan[i].freq&0xffff);
          // also push to the DSP voice so in-emulator playback uses the right rate
          SPC_DSP::voice_t* v=const_cast<SPC_DSP::voice_t*>(dsp.get_voice(i));
          v->ps1_pitch=chan[i].freq&0xffff;
        } else {
          chWrite(i,2,chan[i].freq&0xff);
          chWrite(i,3,chan[i].freq>>8);
        }
        chan[i].freqChanged=false;
      }
    }
  }
  if (koff!=0) {
    if (ps1Mode) {
      // PS1 key-off: only emit words with active bits
      if (koff&0xffff) ps1WriteForce(PS1_REG_KEY_OFF_LO,koff&0xffff);
      if (koff>>16) ps1WriteForce(PS1_REG_KEY_OFF_HI,(koff>>16)&0xffff);
    } else {
      // TODO: improve
      if (antiClick) {
        for (int i=0; i<8; i++) {
          if (koff&(1<<i)) {
            chWrite(i,5,0);
            chWrite(i,7,0x9f);
            chan[i].shallWriteEnv=true;
          }
        }
        rWriteDelay(0x7e,0,64);
      }
      rWriteDelay(0x5c,koff,8);
    }
  }
  if (writeControl) {
    if (ps1Mode) {
      // PS1 SPU noise frequency is in SPUCNT register bits 8-13
      // but for the dump we emit it as a dedicated virtual register
      ps1Write(PS1_REG_NOISE_FREQ,noiseFreq&0x1f);
    } else {
      unsigned char control=(noiseFreq&0x1f)|(echoOn?0:0x20);
      rWrite(0x6c,control);
    }
    writeControl=false;
  }
  if (writeNoise) {
    unsigned int noiseBits=0;
    for (int i=0; i<chanCount; i++) {
      if (chan[i].noise) noiseBits|=(1<<i);
    }
    if (ps1Mode) {
      ps1Write(PS1_REG_NOISE_LO,noiseBits&0xffff);
      ps1Write(PS1_REG_NOISE_HI,(noiseBits>>16)&0xffff);
    } else {
      rWrite(0x3d,noiseBits&0xff);
    }
    writeNoise=false;
  }
  if (writePitchMod) {
    unsigned int pitchModBits=0;
    for (int i=0; i<chanCount; i++) {
      if (chan[i].pitchMod) pitchModBits|=(1<<i);
    }
    if (ps1Mode) {
      ps1Write(PS1_REG_PMOD_LO,pitchModBits&0xffff);
      ps1Write(PS1_REG_PMOD_HI,(pitchModBits>>16)&0xffff);
    } else {
      rWrite(0x2d,pitchModBits&0xff);
    }
    writePitchMod=false;
  }
  if (writeEcho) {
    unsigned int echoBits=0;
    for (int i=0; i<chanCount; i++) {
      if (chan[i].echo) echoBits|=(1<<i);
    }
    if (ps1Mode) {
      // update reverb voice mask in DSP
      SPC_DSP::PS1Reverb rev=dsp.getPS1Reverb();
      rev.voiceMask=echoBits;
      dsp.setPS1Reverb(rev);
      // emit for export
      ps1Write(PS1_REG_REVERB_LO,echoBits&0xffff);
      ps1Write(PS1_REG_REVERB_HI,(echoBits>>16)&0xffff);
    } else {
      rWrite(0x4d,echoBits&0xff);
    }
    writeEcho=false;
  }
  if (writeDryVol) {
    if (ps1Mode) {
      // PS1 main volume: 16-bit signed, 0x3FFF = max
      int mvL=(dryVolL*0x3FFF)/127;
      int mvR=(dryVolR*0x3FFF)/127;
      ps1Write(PS1_REG_MAIN_VOL_L,mvL&0xffff);
      ps1Write(PS1_REG_MAIN_VOL_R,mvR&0xffff);
    } else {
      rWrite(0x0c,dryVolL);
      rWrite(0x1c,dryVolR);
    }
    writeDryVol=false;
  }
  for (int i=0; i<chanCount; i++) {
    if (chan[i].shallWriteEnv) {
      writeEnv(i);
      chan[i].shallWriteEnv=false;
    }
  }
  if (koff!=0 && !ps1Mode) {
    rWriteDelay(0x5c,0,8);
  }
  for (int i=0; i<chanCount; i++) {
    if (chan[i].shallWriteVol) {
      writeOutVol(i);
      chan[i].shallWriteVol=false;
    }
  }
  if (kon!=0) {
    if (ps1Mode) {
      // PS1 key-on: only emit words with active bits
      if (kon&0xffff) ps1WriteForce(PS1_REG_KEY_ON_LO,kon&0xffff);
      if (kon>>16) ps1WriteForce(PS1_REG_KEY_ON_HI,(kon>>16)&0xffff);
    } else {
      rWrite(0x4c,kon);
    }
  }
}

int DivPlatformSNES::dispatch(DivCommand c) {
  switch (c.cmd) {
    case DIV_CMD_NOTE_ON: {
      DivInstrument* ins=parent->getIns(chan[c.chan].ins,ps1Mode?DIV_INS_PS1:DIV_INS_SNES);
      if (!ps1Mode && ins->amiga.useWave) {
        chan[c.chan].useWave=true;
        chan[c.chan].sampleNote=DIV_NOTE_NULL;
        chan[c.chan].sampleNoteDelta=0;
        chan[c.chan].wtLen=ins->amiga.waveLen+1;
        if (chan[c.chan].insChanged) {
          if (chan[c.chan].wave<0) {
            chan[c.chan].wave=0;
          }
          chan[c.chan].ws.setWidth(chan[c.chan].wtLen);
          chan[c.chan].ws.changeWave1(chan[c.chan].wave);
        }
        chan[c.chan].ws.init(ins,chan[c.chan].wtLen,15,chan[c.chan].insChanged);
      } else {
        if (c.value!=DIV_NOTE_NULL) {
          chan[c.chan].sample=ins->amiga.getSample(c.value);
          chan[c.chan].sampleNote=c.value;
          c.value=ins->amiga.getFreq(c.value);
          chan[c.chan].sampleNoteDelta=c.value-chan[c.chan].sampleNote;
        }
        chan[c.chan].useWave=false;
      }
      if (chan[c.chan].useWave || chan[c.chan].sample<0 || chan[c.chan].sample>=parent->song.sampleLen) {
        chan[c.chan].sample=-1;
      }
      if (chan[c.chan].insChanged) {
        if (ps1Mode) {
          chan[c.chan].ps1State=ins->ps1;
          // PS1 has its own native sustain handling - the SNES sus mode is unused
          chan[c.chan].state.sus=0;
        } else {
          chan[c.chan].state=ins->snes;
        }
      }
      chan[c.chan].active=true;
      if (chan[c.chan].insChanged || chan[c.chan].state.sus) {
        chan[c.chan].shallWriteEnv=true;
      }
      if (c.value!=DIV_NOTE_NULL) {
        chan[c.chan].baseFreq=round(NOTE_FREQUENCY(c.value));
        chan[c.chan].freqChanged=true;
        chan[c.chan].note=c.value;
      }
      chan[c.chan].keyOn=true;
      chan[c.chan].macroInit(ins);
      // this is the fix. it needs testing.
      if (!parent->song.compatFlags.brokenOutVol && !chan[c.chan].std.vol.will) {
        if (chan[c.chan].outVol!=chan[c.chan].vol) chan[c.chan].shallWriteVol=true;
        chan[c.chan].outVol=chan[c.chan].vol;
      }
      chan[c.chan].insChanged=false;
      break;
    }
    case DIV_CMD_NOTE_OFF:
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      chan[c.chan].keyOn=false;
      if (chan[c.chan].state.sus) {
        chan[c.chan].shallWriteEnv=true;
      } else {
        chan[c.chan].macroInit(NULL);
      }
      break;
    case DIV_CMD_NOTE_OFF_ENV:
      chan[c.chan].active=false;
      chan[c.chan].keyOff=true;
      chan[c.chan].keyOn=false;
      if (chan[c.chan].state.sus) {
        chan[c.chan].shallWriteEnv=true;
      }
      chan[c.chan].std.release();
      break;
    case DIV_CMD_ENV_RELEASE:
      chan[c.chan].std.release();
      break;
    case DIV_CMD_INSTRUMENT:
      if (chan[c.chan].ins!=c.value || c.value2==1) {
        chan[c.chan].ins=c.value;
        chan[c.chan].insChanged=true;
      }
      break;
    case DIV_CMD_VOLUME:
      if (chan[c.chan].vol!=c.value) {
        chan[c.chan].vol=c.value;
        if (!chan[c.chan].std.vol.has) {
          chan[c.chan].outVol=c.value;
          chan[c.chan].shallWriteVol=true;
        }
      }
      break;
    case DIV_CMD_GET_VOLUME:
      return chan[c.chan].vol;
      break;
    case DIV_CMD_PANNING:
      chan[c.chan].panL=c.value>>1;
      chan[c.chan].panR=c.value2>>1;
      chan[c.chan].shallWriteVol=true;
      break;
    case DIV_CMD_PITCH:
      chan[c.chan].pitch=c.value;
      chan[c.chan].freqChanged=true;
      break;
    case DIV_CMD_WAVE:
      if (!chan[c.chan].useWave) break;
      chan[c.chan].wave=c.value;
      chan[c.chan].ws.changeWave1(chan[c.chan].wave);
      break;
    case DIV_CMD_NOTE_PORTA: {
      int destFreq=round(NOTE_FREQUENCY(c.value2+chan[c.chan].sampleNoteDelta));
      bool return2=false;
      if (destFreq>chan[c.chan].baseFreq) {
        chan[c.chan].baseFreq+=c.value;
        if (chan[c.chan].baseFreq>=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      } else {
        chan[c.chan].baseFreq-=c.value;
        if (chan[c.chan].baseFreq<=destFreq) {
          chan[c.chan].baseFreq=destFreq;
          return2=true;
        }
      }
      chan[c.chan].freqChanged=true;
      if (return2) {
        chan[c.chan].inPorta=false;
        return 2;
      }
      break;
    }
    case DIV_CMD_LEGATO: {
      chan[c.chan].baseFreq=round(NOTE_FREQUENCY(c.value+chan[c.chan].sampleNoteDelta+((HACKY_LEGATO_MESS)?(chan[c.chan].std.arp.val):(0))));
      chan[c.chan].freqChanged=true;
      chan[c.chan].note=c.value;
      break;
    }
    case DIV_CMD_PRE_PORTA:
      if (chan[c.chan].active && c.value2) {
        if (parent->song.compatFlags.resetMacroOnPorta) chan[c.chan].macroInit(parent->getIns(chan[c.chan].ins,ps1Mode?DIV_INS_PS1:DIV_INS_SNES));
      }
      chan[c.chan].inPorta=c.value;
      break;
    case DIV_CMD_SAMPLE_POS:
      chan[c.chan].audPos=c.value;
      chan[c.chan].setPos=true;
      break;
    case DIV_CMD_STD_NOISE_MODE:
      chan[c.chan].noise=c.value;
      writeNoise=true;
      break;
    case DIV_CMD_SNES_PITCH_MOD:
      chan[c.chan].pitchMod=c.value;
      writePitchMod=true;
      break;
    case DIV_CMD_SNES_INVERT:
      chan[c.chan].invertL=(c.value>>4);
      chan[c.chan].invertR=c.value&15;
      chan[c.chan].shallWriteVol=true;
      break;
    case DIV_CMD_SNES_GAIN_MODE:
      if (ps1Mode) break; // no GAIN on PS1
      if (c.value) {
        chan[c.chan].state.useEnv=false;
        switch (c.value) {
          case 1:
            chan[c.chan].state.gainMode=DivInstrumentSNES::GAIN_MODE_DIRECT;
            break;
          case 2:
            chan[c.chan].state.gainMode=DivInstrumentSNES::GAIN_MODE_DEC_LINEAR;
            break;
          case 3:
            chan[c.chan].state.gainMode=DivInstrumentSNES::GAIN_MODE_DEC_LOG;
            break;
          case 4:
            chan[c.chan].state.gainMode=DivInstrumentSNES::GAIN_MODE_INC_LINEAR;
            break;
          case 5:
            chan[c.chan].state.gainMode=DivInstrumentSNES::GAIN_MODE_INC_INVLOG;
            break;
        }
      } else {
        chan[c.chan].state.useEnv=true;
      }
      chan[c.chan].shallWriteEnv=true;
      break;
    case DIV_CMD_SNES_GAIN:
      if (ps1Mode) break; // no GAIN on PS1
      if (chan[c.chan].state.gainMode==DivInstrumentSNES::GAIN_MODE_DIRECT) {
        chan[c.chan].state.gain=c.value&0x7f;
      } else {
        chan[c.chan].state.gain=c.value&0x1f;
      }
      if (!chan[c.chan].state.useEnv) chan[c.chan].shallWriteEnv=true;
      break;
    case DIV_CMD_STD_NOISE_FREQ:
      noiseFreq=c.value&0x1f;
      writeControl=true;
      break;
    case DIV_CMD_FM_AR:
      chan[c.chan].state.a=c.value&15;
      if (chan[c.chan].state.useEnv) chan[c.chan].shallWriteEnv=true;
      break;
    case DIV_CMD_FM_DR:
      chan[c.chan].state.d=c.value&7;
      if (chan[c.chan].state.useEnv) chan[c.chan].shallWriteEnv=true;
      break;
    case DIV_CMD_FM_SL:
      chan[c.chan].state.s=c.value&7;
      if (chan[c.chan].state.useEnv) chan[c.chan].shallWriteEnv=true;
      break;
    case DIV_CMD_FM_RR:
      chan[c.chan].state.r=c.value&0x1f;
      if (chan[c.chan].state.useEnv) chan[c.chan].shallWriteEnv=true;
      break;
    case DIV_CMD_SNES_ECHO:
      chan[c.chan].echo=c.value;
      writeEcho=true;
      break;
    case DIV_CMD_SNES_ECHO_DELAY: {
      echoDelay=c.value&15;
      unsigned char esa=0xf8-(echoDelay<<3);
      if (echoOn) {
        rWrite(0x6d,esa);
        rWrite(0x7d,echoDelay);
      }
      break;
    }
    case DIV_CMD_SNES_ECHO_ENABLE:
      echoOn=c.value;
      initEcho();
      break;
    case DIV_CMD_SNES_ECHO_FEEDBACK:
      echoFeedback=c.value;
      if (echoOn) {
        rWrite(0x0d,echoFeedback);
      }
      break;
    case DIV_CMD_SNES_ECHO_FIR:
      echoFIR[c.value&7]=c.value2;
      if (echoOn) {
        rWrite(0x0f+((c.value&7)<<4),echoFIR[c.value&7]);
      }
      break;
    case DIV_CMD_SNES_ECHO_VOL_LEFT:
      echoVolL=c.value;
      if (echoOn) {
        rWrite(0x2c,echoVolL);
      }
      break;
    case DIV_CMD_SNES_ECHO_VOL_RIGHT:
      echoVolR=c.value;
      if (echoOn) {
        rWrite(0x3c,echoVolR);
      }
      break;
    case DIV_CMD_SNES_GLOBAL_VOL_LEFT:
      dryVolL=c.value;
      writeDryVol=true;
      break;
    case DIV_CMD_SNES_GLOBAL_VOL_RIGHT:
      dryVolR=c.value;
      writeDryVol=true;
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
      break;
  }
  return 1;
}

void DivPlatformSNES::updateWave(int ch) {
  // Due to the overflow bug in hardware's resampler, the written amplitude here is half of maximum
  unsigned short pos=waveTableAddr(ch);
  for (int i=0; i<chan[ch].wtLen/16; i++) {
    sampleMem[pos++]=0xb0;
    for (int j=0; j<8; j++) {
      int nibble1=(chan[ch].ws.output[i*16+j*2]-8)&15;
      int nibble2=(chan[ch].ws.output[i*16+j*2+1]-8)&15;
      sampleMem[pos++]=(nibble1<<4)|nibble2;
    }
  }
  sampleMem[pos-9]=0xb3; // mark loop
}

void DivPlatformSNES::writeOutVol(int ch) {
  int outL=0;
  int outR=0;
  if (!isMuted[ch]) {
    outL=(globalVolL*((chan[ch].outVol*chan[ch].panL)/127))/127;
    outR=(globalVolR*((chan[ch].outVol*chan[ch].panR)/127))/127;
    if (chan[ch].invertL) outL=-outL;
    if (chan[ch].invertR) outR=-outR;
  }
  if (ps1Mode) {
    // PS1 SPU: voice volume registers are 16-bit signed
    // scale to SPU range (0x0000-0x3FFF)
    int spuVolL=(outL*0x3FFF)/127;
    int spuVolR=(outR*0x3FFF)/127;
    if (spuVolL>0x3FFF) spuVolL=0x3FFF;
    if (spuVolL<-0x3FFF) spuVolL=-0x3FFF;
    if (spuVolR>0x3FFF) spuVolR=0x3FFF;
    if (spuVolR<-0x3FFF) spuVolR=-0x3FFF;
    // set on DSP voice for playback
    SPC_DSP::voice_t* v=const_cast<SPC_DSP::voice_t*>(dsp.get_voice(ch));
    v->out[0]=spuVolL>>7; // scale for DSP output mixing
    v->out[1]=spuVolR>>7;
    // emit register writes for export
    ps1ChWrite(ch,PS1_REG_VOL_L,spuVolL&0xffff);
    ps1ChWrite(ch,PS1_REG_VOL_R,spuVolR&0xffff);
    return;
  }
  chWrite(ch,0,outL);
  chWrite(ch,1,outR);
}

void DivPlatformSNES::writeEnv(int ch) {
  if (ps1Mode) {
    // PS1 SPU ADSR register layout (psx-spx):
    //   ADSR_LO (0x08): sustainLevel:4 | decay:4 | attack:7 | attackMode:1
    //   ADSR_HI (0x0A): release:5 | releaseMode:1 | sustainRate:7 | reserved:1 | sustainDir:1 | sustainMode:1
    // Rate fields: 0=fastest..max=slowest. Modes: 0=linear, 1=exponential.
    const DivInstrumentPS1& p=chan[ch].ps1State;
    unsigned short adsrLo=(p.s&0xf)
                         |((p.d&0xf)<<4)
                         |((p.a&0x7f)<<8)
                         |((p.aExp?1:0)<<15);
    unsigned short adsrHi=(p.r&0x1f)
                         |((p.rExp?1:0)<<5)
                         |((p.sr&0x7f)<<6)
                         |((p.sDir?1:0)<<14)
                         |((p.sExp?1:0)<<15);

    // emit register writes for export
    ps1ChWrite(ch,PS1_REG_ADSR_LO,adsrLo);
    ps1ChWrite(ch,PS1_REG_ADSR_HI,adsrHi);

    // and push to the DSP voice so in-emulator playback uses the same values
    SPC_DSP::voice_t* v=const_cast<SPC_DSP::voice_t*>(dsp.get_voice(ch));
    v->ps1_adsr1=adsrLo;
    v->ps1_adsr2=adsrHi;
    return;
  }
  if (chan[ch].state.useEnv) {
    if (chan[ch].state.sus) {
      if (chan[ch].active) {
        chWrite(ch,5,chan[ch].state.a|(chan[ch].state.d<<4)|0x80);
        chWrite(ch,6,(chan[ch].state.s<<5)|(chan[ch].state.d2&31));
      } else {
        switch (chan[ch].state.sus) {
          case 1: // dec linear
            chWrite(ch,7,0x80|chan[ch].state.r);
            chWrite(ch,5,0);
            break;
          case 2: // dec exp
            chWrite(ch,7,0xa0|chan[ch].state.r);
            chWrite(ch,5,0);
            break;
          case 3: // update r
            chWrite(ch,6,(chan[ch].state.s<<5)|(chan[ch].state.r&31));
            break;
          default: // what?
            break;
        }
      }
    } else {
      chWrite(ch,5,chan[ch].state.a|(chan[ch].state.d<<4)|0x80);
      chWrite(ch,6,chan[ch].state.r|(chan[ch].state.s<<5));
    }
  } else {
    chWrite(ch,5,0);
    switch (chan[ch].state.gainMode) {
      case DivInstrumentSNES::GAIN_MODE_DIRECT:
        chWrite(ch,7,chan[ch].state.gain&127);
        break;
      case DivInstrumentSNES::GAIN_MODE_DEC_LINEAR:
        chWrite(ch,7,0x80|(chan[ch].state.gain&31));
        break;
      case DivInstrumentSNES::GAIN_MODE_INC_LINEAR:
        chWrite(ch,7,0xc0|(chan[ch].state.gain&31));
        break;
      case DivInstrumentSNES::GAIN_MODE_DEC_LOG:
        chWrite(ch,7,0xa0|(chan[ch].state.gain&31));
        break;
      case DivInstrumentSNES::GAIN_MODE_INC_INVLOG:
        chWrite(ch,7,0xe0|(chan[ch].state.gain&31));
        break;
    }
  }
}

void DivPlatformSNES::muteChannel(int ch, bool mute) {
  isMuted[ch]=mute;
  chan[ch].shallWriteVol=true;
}

void DivPlatformSNES::forceIns() {
  for (int i=0; i<chanCount; i++) {
    chan[i].insChanged=true;
    chan[i].freqChanged=true;
    chan[i].sample=-1;
    if (!ps1Mode && chan[i].active && chan[i].useWave) {
      updateWave(i);
    }
    if (ps1Mode && chan[i].active) {
      // force re-send envelope state for active PS1 voices
      chan[i].shallWriteEnv=true;
      // invalidate register cache for this voice so all writes go through
      memset(&ps1RegCache[i*0x10],0xff,0x10);
    }
    writeOutVol(i);
  }
  writeControl=true;
  writeNoise=true;
  writePitchMod=true;
  writeEcho=!ps1Mode;
  writeDryVol=true;
  if (!ps1Mode) {
    initEcho();
  }
  if (ps1Mode) {
    // invalidate global register cache
    for (int i=0x180; i<0x1A0; i++) {
      ps1RegCache[i]=0xffff;
    }
  }
}

void* DivPlatformSNES::getChanState(int ch) {
  return &chan[ch];
}

DivMacroInt* DivPlatformSNES::getChanMacroInt(int ch) {
  return &chan[ch].std;
}

unsigned short DivPlatformSNES::getPan(int ch) {
  return (chan[ch].panL<<8)|chan[ch].panR;
}

void DivPlatformSNES::getPaired(int ch, std::vector<DivChannelPair>& ret) {
  if (chan[ch].pitchMod) {
    ret.push_back(DivChannelPair(_("mod"),(ch-1)&7));
  }
}

DivChannelModeHints DivPlatformSNES::getModeHints(int ch) {
  DivChannelModeHints ret;
  ret.count=1;
  ret.hint[0]="-";
  ret.type[0]=0;

  const SPC_DSP::voice_t* v=dsp.get_voice(ch);
  if (v!=NULL && v->regs!=NULL) {
    if (v->regs[5]&128) {
      switch (v->env_mode) {
        case SPC_DSP::env_attack:
          ret.hint[0]=ICON_FUR_ADSR_A;
          ret.type[0]=12;
          break;
        case SPC_DSP::env_decay:
          ret.hint[0]=ICON_FUR_ADSR_D;
          ret.type[0]=13;
          break;
        case SPC_DSP::env_sustain:
          ret.hint[0]=ICON_FUR_ADSR_S;
          ret.type[0]=14;
          break;
        case SPC_DSP::env_release:
          ret.hint[0]=ICON_FUR_ADSR_R;
          ret.type[0]=15;
          break;
      }
    } else {
      if (v->regs[7]&128) {
        switch (v->regs[7]&0x60) {
          case 0:
            ret.hint[0]=ICON_FUR_DEC_LINEAR;
            ret.type[0]=16;
            break;
          case 32:
            ret.hint[0]=ICON_FUR_DEC_EXP;
            ret.type[0]=17;
            break;
          case 64:
            ret.hint[0]=ICON_FUR_INC_LINEAR;
            ret.type[0]=18;
            break;
          case 96:
            ret.hint[0]=ICON_FUR_INC_BENT;
            ret.type[0]=19;
            break;
        }
      } else {
        ret.hint[0]=ICON_FUR_VOL_DIRECT;
        ret.type[0]=20;
      }
    }
  }
  
  return ret;
}

DivSamplePos DivPlatformSNES::getSamplePos(int ch) {
  if (ch>=chanCount) return DivSamplePos();
  if (!chan[ch].active) return DivSamplePos();
  if (chan[ch].sample<0 || chan[ch].sample>=parent->song.sampleLen) return DivSamplePos();
  const SPC_DSP::voice_t* v=dsp.get_voice(ch);
  if (ps1Mode) {
    return DivSamplePos(
      chan[ch].sample,
      ((v->brr_addr-sampleOff[chan[ch].sample])/16*28)+v->brr_offset*2,
      (chan[ch].freq*125)/16
    );
  }
  // TODO: fix?
  if (sampleMem[v->brr_addr&0xffff]==0) return DivSamplePos();
  return DivSamplePos(
    chan[ch].sample,
    ((v->brr_addr-sampleOff[chan[ch].sample])*16/9)+v->brr_offset,
    (chan[ch].freq*125)/16
  );
}

DivDispatchOscBuffer* DivPlatformSNES::getOscBuffer(int ch) {
  return oscBuf[ch];
}

unsigned char* DivPlatformSNES::getRegisterPool() {
  // get states from emulator
  for (int i=0; i<0x80; i+=0x10) {
    regPool[i+8]=dsp.read(i+8);
    regPool[i+9]=dsp.read(i+9);
  }
  regPool[0x7c]=dsp.read(0x7c); // ENDX
  return regPool;
}

int DivPlatformSNES::getRegisterPoolSize() {
  return 128;
}

void DivPlatformSNES::initEcho() {
  unsigned char esa=0xf8-(echoDelay<<3);
  unsigned char control=(noiseFreq&0x1f)|(echoOn?0:0x20);
  if (echoOn) {
    rWrite(0x6d,esa);
    rWrite(0x7d,echoDelay);
    rWrite(0x0d,echoFeedback);
    rWrite(0x2c,echoVolL);
    rWrite(0x3c,echoVolR);
    for (int i=0; i<8; i++) {
      rWrite(0x0f+(i<<4),echoFIR[i]);
    }
    rWrite(0x6c,control);
  } else {
    rWrite(0x2c,0);
    rWrite(0x3c,0);
    rWrite(0x6c,control);
    rWrite(0x7d,0);
    rWrite(0x6d,0xff);
  }

  for (DivMemoryEntry& i: memCompo.entries) {
    if (i.type==DIV_MEMORY_ECHO) {
      i.begin=(0xf800-echoDelay*2048);
    }
  }
  memCompo.used=sampleMemLen+echoDelay*2048;
}

void DivPlatformSNES::reset() {
  writes.clear();

  memcpy(sampleMem,copyOfSampleMem,sampleMemSize);

  if (ps1Mode) {
    dsp.initPS1(sampleMem);
    // invalidate register cache so first writes always emit
    memset(ps1RegCache,0xff,sizeof(ps1RegCache));
  } else {
    dsp.init(sampleMem);
  }
  dsp.set_output(NULL,0);
  dsp.setupInterpolation(!interpolationOff);

  memset(regPool,0,128);

  if (!ps1Mode) {
    // SNES-specific initialization
    sampleTableBase=0x400;
    rWrite(0x5d,sampleTableBase>>8);
    rWrite(0x0c,127); // global volume left
    rWrite(0x1c,127); // global volume right
    rWrite(0x6c,0); // get DSP out of reset
  }

  for (int i=0; i<chanCount; i++) {
    chan[i]=Channel();
    chan[i].std.setEngine(parent);
    if (!ps1Mode) {
      chan[i].ws.setEngine(parent);
      chan[i].ws.init(NULL,32,15);
    }
    writeOutVol(i);
    if (!ps1Mode) {
      chWrite(i,4,i); // source number
    }
  }
  writeControl=false;
  writeNoise=false;
  writePitchMod=false;
  writeEcho=!ps1Mode;
  writeDryVol=false;

  dryVolL=127;
  dryVolR=127;

  if (!ps1Mode) {
    echoDelay=initEchoDelay;
    echoFeedback=initEchoFeedback;
    echoFIR[0]=initEchoFIR[0];
    echoFIR[1]=initEchoFIR[1];
    echoFIR[2]=initEchoFIR[2];
    echoFIR[3]=initEchoFIR[3];
    echoFIR[4]=initEchoFIR[4];
    echoFIR[5]=initEchoFIR[5];
    echoFIR[6]=initEchoFIR[6];
    echoFIR[7]=initEchoFIR[7];
    echoVolL=initEchoVolL;
    echoVolR=initEchoVolR;
    echoOn=initEchoOn;

    for (int i=0; i<8; i++) {
      if (initEchoMask&(1<<i)) {
        chan[i].echo=true;
      }
    }

    initEcho();
  }

  if (ps1Mode && ps1ReverbEnabled && ps1ReverbPreset>0 && ps1ReverbPreset<PS1_REVERB_PRESET_COUNT) {
    // apply reverb preset to DSP
    SPC_DSP::PS1Reverb rev;
    const short* p=ps1ReverbPresets[ps1ReverbPreset];
    rev.enabled=true;
    rev.dAPF1=p[0]; rev.dAPF2=p[1];
    rev.vIIR=p[2]; rev.vCOMB1=p[3]; rev.vCOMB2=p[4]; rev.vCOMB3=p[5]; rev.vCOMB4=p[6]; rev.vWALL=p[7];
    rev.vAPF1=p[8]; rev.vAPF2=p[9];
    rev.mLSAME=p[10]; rev.mRSAME=p[11]; rev.mLCOMB1=p[12]; rev.mRCOMB1=p[13]; rev.mLCOMB2=p[14]; rev.mRCOMB2=p[15];
    rev.dLSAME=p[16]; rev.dRSAME=p[17]; rev.mLDIFF=p[18]; rev.mRDIFF=p[19]; rev.mLCOMB3=p[20]; rev.mRCOMB3=p[21]; rev.mLCOMB4=p[22]; rev.mRCOMB4=p[23];
    rev.dLDIFF=p[24]; rev.dRDIFF=p[25]; rev.mLAPF1=p[26]; rev.mRAPF1=p[27]; rev.mLAPF2=p[28]; rev.mRAPF2=p[29];
    rev.vLIN=p[30]; rev.vRIN=p[31];
    rev.vLOUT=ps1ReverbVolL;
    rev.vROUT=ps1ReverbVolR;
    // reverb buffer at end of sample memory
    unsigned int reverbSize=ps1ReverbSizes[ps1ReverbPreset];
    rev.bufferBase=sampleMemSize-reverbSize;
    rev.bufferAddr=rev.bufferBase;
    rev.voiceMask=0; // per-voice enable set by echo flag
    for (int i=0; i<chanCount; i++) {
      if (chan[i].echo) rev.voiceMask|=(1<<i);
    }
    dsp.setPS1Reverb(rev);
    // zero-fill reverb buffer
    memset(&sampleMem[rev.bufferBase],0,reverbSize);
  }
}

int DivPlatformSNES::getOutputCount() {
  return 2;
}

bool DivPlatformSNES::hasSoftPan(int ch) {
  return true;
}

void DivPlatformSNES::notifyInsChange(int ins) {
  for (int i=0; i<chanCount; i++) {
    if (chan[i].ins==ins) {
      chan[i].insChanged=true;
    }
  }
}

void DivPlatformSNES::notifyWaveChange(int wave) {
  for (int i=0; i<chanCount; i++) {
    if (chan[i].useWave && chan[i].wave==wave) {
      chan[i].ws.changeWave1(wave);
      if (chan[i].active) {
        updateWave(i);
      }
    }
  }
}

void DivPlatformSNES::notifyInsDeletion(void* ins) {
  for (int i=0; i<chanCount; i++) {
    chan[i].std.notifyInsDeletion((DivInstrument*)ins);
  }
}

void DivPlatformSNES::poke(unsigned int addr, unsigned short val) {
  rWrite(addr,val);
}

void DivPlatformSNES::poke(std::vector<DivRegWrite>& wlist) {
  for (DivRegWrite& i: wlist) rWrite(i.addr,i.val);
}

const void* DivPlatformSNES::getSampleMem(int index) {
  return index == 0 ? sampleMem : NULL;
}

size_t DivPlatformSNES::getSampleMemCapacity(int index) {
  if (index!=0) return 0;
  if (ps1Mode) return sampleMemSize; // 512KB, no echo buffer in v1
  return (0xf800-echoDelay*2048);
}

size_t DivPlatformSNES::getSampleMemUsage(int index) {
  return index == 0 ? sampleMemLen : 0;
}

bool DivPlatformSNES::hasSamplePtrHeader(int index) {
  return true;
}

bool DivPlatformSNES::isSampleLoaded(int index, int sample) {
  if (index!=0) return false;
  if (sample<0 || sample>32767) return false;
  return sampleLoaded[sample];
}

const DivMemoryComposition* DivPlatformSNES::getMemCompo(int index) {
  if (index!=0) return NULL;
  return &memCompo;
}

const void* DivPlatformSNES::compileSampleMem(int index, size_t& size) {
  if (ps1Mode) {
    size=sampleMemLen;
    unsigned char* ret=new unsigned char[size];
    memcpy(ret,copyOfSampleMem,size);
    return ret;
  }
  size=MIN(sampleMemLen,(size_t)65536)-sampleTableBase;
  unsigned char* ret=new unsigned char[size];
  memcpy(ret,&copyOfSampleMem[sampleTableBase],size);
  return ret;
}

void DivPlatformSNES::renderSamples(int sysID) {
  memset(copyOfSampleMem,0,sampleMemSize);
  memset(sampleOff,0,32768*sizeof(unsigned int));
  memset(sampleLoaded,0,32768*sizeof(bool));

  memCompo=DivMemoryComposition();

  if (ps1Mode) {
    // PS1 SPU mode: linear sample layout, no directory table, no echo buffer
    memCompo.name="SPU Memory";
    size_t memPos=0;

    for (int i=0; i<parent->song.sampleLen; i++) {
      DivSample* s=parent->song.sample[i];
      if (!s->renderOn[0][sysID]) {
        sampleOff[i]=0;
        continue;
      }

      int length=s->lengthPS1SPU;
      int actualLength=MIN((int)(getSampleMemCapacity()-memPos),length);
      // align to 16-byte blocks
      actualLength=(actualLength/16)*16;
      if (actualLength>0) {
        sampleOff[i]=memPos;
        memcpy(&copyOfSampleMem[memPos],s->dataPS1SPU,actualLength);
        // set loop/end flags in the last block
        if (actualLength>=16) {
          if (s->loop) {
            copyOfSampleMem[memPos+actualLength-16+1]|=0x03; // loop end + repeat
            // set loop start flag on the block containing loopStart
            int loopBlock=(s->loopStart/28)*16;
            if (loopBlock<actualLength) {
              copyOfSampleMem[memPos+loopBlock+1]|=0x04; // loop start
            }
          } else {
            copyOfSampleMem[memPos+actualLength-16+1]|=0x01; // end + mute
          }
        }
        memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_SAMPLE,"Sample",i,memPos,memPos+actualLength));
        memPos+=actualLength;
      }
      if (actualLength<length) {
        logW("out of SPU memory for sample %d!",i);
        break;
      }
      sampleLoaded[i]=true;
    }
    sampleMemLen=memPos;

    memCompo.capacity=sampleMemSize;
    memCompo.used=sampleMemLen;
    memcpy(sampleMem,copyOfSampleMem,sampleMemSize);
  } else {
    // SNES mode: BRR with sample directory table and echo buffer
    memCompo.name="SPC/DSP Memory";

    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_RESERVED,"State",-1,0,sampleTableBase));
    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_RESERVED,"Channel Sample Pointers",-1,sampleTableBase,sampleTableBase+8*4));
    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_WAVE_RAM,"Wave RAM",-1,sampleTableBase+8*4,sampleTableBase+8*4+8*9*16));

    // skip past sample table and wavetable buffer
    size_t memPos=sampleTableBase+8*4+8*9*16;
    size_t sampleTablePos=memPos;

    // allocate sample table
    int maxSample=0;
    for (int i=0; i<parent->song.sampleLen; i++) {
      DivSample* s=parent->song.sample[i];
      if (!s->renderOn[0][sysID]) {
        continue;
      }
      maxSample=i;
    }
    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_RESERVED,"Sample Directory",-1,memPos,memPos+(maxSample+1)*4));
    memPos+=(maxSample+1)*4;

    // write samples
    for (int i=0; i<parent->song.sampleLen; i++) {
      DivSample* s=parent->song.sample[i];
      if (!s->renderOn[0][sysID]) {
        sampleOff[i]=0;
        continue;
      }

      int length=s->lengthBRR+((s->loop && s->depth!=DIV_SAMPLE_DEPTH_BRR)?9:0);
      int actualLength=MIN((int)(getSampleMemCapacity()-memPos)/9*9,length);
      if (actualLength>0) {
        sampleOff[i]=memPos;
        memcpy(&copyOfSampleMem[memPos],s->dataBRR,actualLength);
        // inject loop if needed
        if (s->loop) {
          copyOfSampleMem[memPos+actualLength-9]|=3;
        } else {
          copyOfSampleMem[memPos+actualLength-9]&=~3;
          copyOfSampleMem[memPos+actualLength-9]|=1;
        }
        memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_SAMPLE,"Sample",i,memPos,memPos+actualLength));
        memPos+=actualLength;
      }
      if (actualLength<length) {
        // terminate the sample
        copyOfSampleMem[memPos-9]=1;
        logW("out of BRR memory for sample %d!",i);
        break;
      }
      sampleLoaded[i]=true;
    }
    sampleMemLen=memPos;

    // finish sample table
    for (int i=0; i<=maxSample; i++) {
      if (i>=parent->song.sampleLen) break;
      DivSample* s=parent->song.sample[i];
      if (!s->renderOn[0][sysID]) {
        // unavailable
        copyOfSampleMem[sampleTablePos+i*4]=0;
        copyOfSampleMem[sampleTablePos+i*4+1]=0;
        copyOfSampleMem[sampleTablePos+i*4+2]=0;
        copyOfSampleMem[sampleTablePos+i*4+3]=0;
        continue;
      }

      int start=sampleOff[i];
      int end=MIN(start+MAX(s->lengthBRR+((s->loop && s->depth!=DIV_SAMPLE_DEPTH_BRR)?9:0),1),getSampleMemCapacity());
      int loop=MAX(start,end-1);
      if (s->isLoopable()) {
        loop=((s->depth!=DIV_SAMPLE_DEPTH_BRR)?9:0)+start+((s->loopStart/16)*9);
      }

      copyOfSampleMem[sampleTablePos+i*4]=start&0xff;
      copyOfSampleMem[sampleTablePos+i*4+1]=start>>8;
      copyOfSampleMem[sampleTablePos+i*4+2]=loop&0xff;
      copyOfSampleMem[sampleTablePos+i*4+3]=loop>>8;
    }

    // even if the delay is 0, the DSP will still operate the first buffer sample
    // so the ARAM buffer size becomes 4 bytes when the delay is 0
    memCompo.entries.push_back(DivMemoryEntry(DIV_MEMORY_ECHO,"Echo Buffer",-1,(0xf800-echoDelay*2048),echoDelay==0?0xf804:0xf800));

    memCompo.capacity=65536;
    memCompo.used=sampleMemLen+echoDelay*2048;
    memcpy(sampleMem,copyOfSampleMem,65536);
  }
}

void DivPlatformSNES::setFlags(const DivConfig& flags) {
  globalVolL=127-flags.getInt("volScaleL",0);
  globalVolR=127-flags.getInt("volScaleR",0);

  initEchoOn=flags.getBool("echo",false);
  initEchoVolL=flags.getInt("echoVolL",127);
  initEchoVolR=flags.getInt("echoVolR",127);
  initEchoDelay=flags.getInt("echoDelay",0)&15;
  initEchoFeedback=flags.getInt("echoFeedback",0);

  initEchoFIR[0]=flags.getInt("echoFilter0",127);
  initEchoFIR[1]=flags.getInt("echoFilter1",0);
  initEchoFIR[2]=flags.getInt("echoFilter2",0);
  initEchoFIR[3]=flags.getInt("echoFilter3",0);
  initEchoFIR[4]=flags.getInt("echoFilter4",0);
  initEchoFIR[5]=flags.getInt("echoFilter5",0);
  initEchoFIR[6]=flags.getInt("echoFilter6",0);
  initEchoFIR[7]=flags.getInt("echoFilter7",0);

  initEchoMask=flags.getInt("echoMask",0);

  interpolationOff=flags.getBool("interpolationOff",false);
  antiClick=flags.getBool("antiClick",true);

  if (ps1Mode) {
    ps1ReverbPreset=flags.getInt("ps1ReverbPreset",0);
    ps1ReverbEnabled=(ps1ReverbPreset>0);
    ps1ReverbVolL=flags.getInt("ps1ReverbVolL",0x3FFF);
    ps1ReverbVolR=flags.getInt("ps1ReverbVolR",0x3FFF);
    writePS1Reverb=true;
  }
}

int DivPlatformSNES::init(DivEngine* p, int channels, int sugRate, const DivConfig& flags) {
  parent=p;
  dumpWrites=false;
  skipRegisterWrites=false;
  sampleMemLen=0;

  chanCount=ps1Mode?24:8;
  sampleMemSize=ps1Mode?524288:65536;

  if (sampleMem!=NULL) delete[] sampleMem;
  if (copyOfSampleMem!=NULL) delete[] copyOfSampleMem;
  sampleMem=new signed char[sampleMemSize];
  copyOfSampleMem=new signed char[sampleMemSize];
  memset(sampleMem,0,sampleMemSize);
  memset(copyOfSampleMem,0,sampleMemSize);

  chipClock=ps1Mode?768000:1024000;
  rate=ps1Mode?44100:(chipClock/32);
  for (int i=0; i<chanCount; i++) {
    oscBuf[i]=new DivDispatchOscBuffer;
    oscBuf[i]->setRate(rate);
    isMuted[i]=false;
  }
  setFlags(flags);
  reset();
  return chanCount;
}

void DivPlatformSNES::quit() {
  for (int i=0; i<chanCount; i++) {
    delete oscBuf[i];
  }
}

void DivPlatformSNES::setPS1Mode(bool enabled) {
  ps1Mode=enabled;
}

// initialization of important arrays
DivPlatformSNES::DivPlatformSNES():
  ps1Mode(false),
  chanCount(8),
  sampleMem(NULL),
  copyOfSampleMem(NULL),
  sampleMemSize(0) {
  sampleOff=new unsigned int[32768];
  sampleLoaded=new bool[32768];
}

DivPlatformSNES::~DivPlatformSNES() {
  delete[] sampleOff;
  delete[] sampleLoaded;
  if (sampleMem!=NULL) delete[] sampleMem;
  if (copyOfSampleMem!=NULL) delete[] copyOfSampleMem;
}
