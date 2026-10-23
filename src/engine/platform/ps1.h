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

#ifndef _PS1_H
#define _PS1_H

#include "../dispatch.h"
#include "../macroInt.h"
#include "sound/ps1spu/bridge.h"

// Sony PlayStation SPU, emulated by the pcsx-redux core (sound/ps1spu).
// note on/off with samples, per-instrument ADSR, volume, panning, and the per-voice
// reverb/noise/pitch-mod enable bits. everything else is not implemented yet.
class DivPlatformPS1: public DivDispatch {
  struct Channel: public SharedChannel {
    int sample;
    bool volumeChanged;
    unsigned char panL, panR;
    unsigned short adsr1, adsr2;
    bool reverb, noise, pitchMod;
    Channel(bool linear=true):
      SharedChannel(127,linear),
      sample(-1),
      volumeChanged(false),
      panL(255),
      panR(255),
      adsr1(0x000f),
      adsr2(0x0008),
      reverb(false),
      noise(false),
      pitchMod(false) {}
  };
  Channel chan[24];
  DivDispatchOscBuffer* oscBuf[24];
  bool isMuted[24];
  DivPitchTableManager samplePitchTable;

  PS1SPUBridge* spu;

  unsigned char* sampleMem;
  size_t sampleMemLen;
  unsigned int* sampleOff;
  bool* sampleLoaded;
  DivMemoryComposition memCompo;
  unsigned short regPool[256];
  // per-voice enable bits (bit n = voice n), as last written to PMON/NON/EON.
  unsigned int pmonMask, nonMask, eonMask;

  friend void putDispatchChip(void*,int);
  friend void putDispatchChan(void*,int,int);

  void spuWrite(unsigned int addr, unsigned short val);
  void voiceWrite(int ch, unsigned int reg, unsigned short val);
  void uploadSamples();
  void writeMask(unsigned int regLo, unsigned int& mask, int ch, bool on, bool force=false);
  public:
    void acquire(short** buf, size_t len);
    int dispatch(DivCommand c);
    SharedChannel* getChanState(int chan);
    DivMacroInt* getChanMacroInt(int ch);
    DivDispatchOscBuffer* getOscBuffer(int chan);
    unsigned char* getRegisterPool();
    int getRegisterPoolSize();
    int getRegisterPoolDepth();
    void reset();
    void forceIns();
    void tick(bool sysTick=true);
    void muteChannel(int ch, bool mute);
    int getOutputCount();
    void notifyInsChange(int ins);
    void notifyInsDeletion(void* ins);
    void notifyPitchTable(int sample=-1);
    unsigned int getMaxFreq(int ch);
    void setFlags(const DivConfig& flags);
    void poke(unsigned int addr, unsigned short val);
    void poke(std::vector<DivRegWrite>& wlist);
    const void* getSampleMem(int index=0);
    size_t getSampleMemCapacity(int index=0);
    size_t getSampleMemUsage(int index=0);
    bool isSampleLoaded(int index, int sample);
    const DivMemoryComposition* getMemCompo(int index);
    void renderSamples(int chipID);
    int init(DivEngine* parent, int channels, int sugRate, const DivConfig& flags);
    void quit();
    DivPlatformPS1();
    ~DivPlatformPS1();
};

#endif
