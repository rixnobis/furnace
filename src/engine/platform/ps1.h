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
// note on/off with samples, per-instrument ADSR, volume, panning, the per-voice
// reverb/noise/pitch-mod enable bits, reverb presets (system flags) and the noise clock (1Dxx).
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
  // system flags: reverb preset (index into the psx-spx preset table, 0 = off,
  // REVERB_CUSTOM = registers edited), the reverb output volume (vLOUT/vROUT = vol<<8,
  // -128..127), and the resolved register set (see reverbResolve).
  int reverbPreset;
  int reverbVolL, reverbVolR;
  unsigned short rvbRegs[32];
  unsigned int rvbBase, rvbRequired;
  int rvbWarn;
  // samples that did not fit below the work area in the last renderSamples.
  int rvbSamplesDropped;
  // SPUCNT bits 13-8: noise clock (shift<<2 | step).
  unsigned char noiseFreq;

  friend void putDispatchChip(void*,int);
  friend void putDispatchChan(void*,int,int);

  void spuWrite(unsigned int addr, unsigned short val);
  void voiceWrite(int ch, unsigned int reg, unsigned short val);
  void uploadSamples();
  void writeMask(unsigned int regLo, unsigned int& mask, int ch, bool on, bool force=false);
  void writeCtrl();
  void writeReverb();
  unsigned int reverbBase();
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
    bool hasSamplePtrHeader(int index=0);
    const DivMemoryComposition* getMemCompo(int index);
    void renderSamples(int chipID);
    int init(DivEngine* parent, int channels, int sugRate, const DivConfig& flags);
    void quit();
    static const int REVERB_PRESETS=10;
    // "reverbPreset" value once any reverb register has been edited (reverb on).
    static const int REVERB_CUSTOM=10;
    // reverbResolve warnings
    enum {
      REVERB_WARN_CLAMPED=1, // base + required size ran past the end of SPU RAM: base lowered
      REVERB_WARN_LOW=2,     // base was inside the capture buffers: raised to the sample area
      REVERB_WARN_ALIAS=4    // the required size does not fit above the capture buffers at all
    };
    struct ReverbSetup {
      int preset;
      unsigned short regs[32];
      // work area start in bytes (multiple of 8), bytes the registers address (derived),
      // REVERB_WARN_* bits. Off: base=RAM_SIZE, required=0.
      unsigned int base, required;
      int warn;
    };
    // preset name, in flag order ("reverbPreset" 0..REVERB_PRESETS-1, REVERB_CUSTOM)
    static const char* reverbPresetName(int preset);
    // work area size in bytes of a preset as listed by psx-spx (0 for off)
    static unsigned int reverbSize(int preset);
    // psx-spx register name (index 0..31 = 1F801DC0h..1F801DFEh); flag key is "rvb_"+name
    static const char* reverbRegName(int reg);
    static unsigned short reverbPresetReg(int preset, int reg);
    // whether register reg is an address (dAPF/m*/d*, 8-byte units) rather than a volume
    static bool reverbRegIsAddr(int reg);
    // bytes of work area the address registers reach (see ps1.cpp)
    static unsigned int reverbRequired(const unsigned short* regs);
    // flags -> register set, base and warnings (the loading rule, see ps1.cpp)
    static void reverbResolve(const DivConfig& flags, ReverbSetup& s);
    void toggleRegisterDump(bool enable);
    // SPU RAM byte address where renderSamples starts placing samples (after the capture buffers)
    unsigned int getSampleBase();
    // SPU RAM byte address of the reverb work area (RAM_SIZE when reverb is off)
    unsigned int getReverbBase();
    int reverbWarnings() { return rvbWarn; }
    int reverbSamplesDropped() { return rvbSamplesDropped; }
    DivPlatformPS1();
    ~DivPlatformPS1();
};

#endif
