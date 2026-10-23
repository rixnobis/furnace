// Ours. C++14-safe front for the vendored (C++20) pcsx-redux SPU core.
// Furnace code includes only this header; bridge.cpp is the only TU that sees the core.
#ifndef _PS1SPU_BRIDGE_H
#define _PS1SPU_BRIDGE_H

#include <stddef.h>

// Each instance is a self-contained core, run synchronously on the calling thread (no mixer
// thread). One instance must not be called from two threads at once; distinct instances may.
class PS1SPUBridge {
  struct Impl;
  Impl* p;

  public:
    // SPU sound RAM size in bytes.
    static const unsigned int RAM_SIZE=512*1024;
    // Output rate of the core, in Hz.
    static const unsigned int RATE=44100;

    // reset the core: stops the mixer, clears voices and timing, restarts at sample 0.
    // sound RAM is kept.
    void reset();
    // write/read an SPU register. reg is the hardware address (0x1F801C00-0x1F801DFF) or just
    // its low 12 bits. takes effect at the current output sample.
    void writeReg(unsigned int reg, unsigned short val);
    unsigned short readReg(unsigned int reg);
    // copy len bytes into sound RAM at addr, through the transfer address/DMA path.
    // addr must be a multiple of 8, len a multiple of 2.
    void writeRAM(unsigned int addr, const unsigned char* data, size_t len);
    // advance the core by frames output samples (44.1 kHz) and copy them out.
    void render(short* outL, short* outR, size_t frames);

    // encode 16-bit PCM to SPU ADPCM. loopStart<0 means one-shot; otherwise samples
    // [loopStart,loopEnd) repeat (both rounded to 28-sample blocks).
    // returns the number of bytes written (always a multiple of 16), or 0 if outCap is too small.
    static size_t encodeADPCM(const short* in, size_t len, int loopStart, int loopEnd, unsigned char* out, size_t outCap);
    // upper bound of encodeADPCM's output size for len samples.
    static size_t encodedSize(size_t len);

    PS1SPUBridge();
    ~PS1SPUBridge();
};

#endif
