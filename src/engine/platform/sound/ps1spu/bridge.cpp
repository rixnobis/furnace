// Ours. See bridge.h. This TU is C++20, like the vendored core.
//
// The core is driven synchronously: render() runs the mixer on the caller's thread, one
// instance at a time, with no mixer thread and no shared lock, through the core's embedding
// API (impl::openSynchronous / impl::mixSynchronous).
#include "bridge.h"

#include <string.h>

#include <algorithm>
#include <vector>

#include "core/psxemulator.h"
#include "spu/interface.h"
#include "spu/registers.h"
#include "supportpsx/adpcm.h"

namespace {

using PCSX::SPU::impl;

// The core's clock: 768 CPU cycles per 44.1 kHz sample.
constexpr uint64_t kCyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;
// Interleaved scratch for one render step.
constexpr size_t kMaxRenderChunk = 4096;

}  // namespace

struct PS1SPUBridge::Impl {
    // this instance's CPU cycle counter, seen by the core through PCSX::g_emulator
    PCSX::Emulator emu;
    impl* spu = nullptr;
    // output samples handed to the caller so far
    uint64_t delivered = 0;
    int16_t scratch[kMaxRenderChunk * 2];

    // Points the core's (thread_local) g_emulator at this instance for the scope.
    struct Use {
        PCSX::Emulator* prev;
        explicit Use(Impl* p) : prev(PCSX::g_emulator) { PCSX::g_emulator = &p->emu; }
        ~Use() { PCSX::g_emulator = prev; }
        Use(const Use&) = delete;
        Use& operator=(const Use&) = delete;
    };

    // A write made "now" is stamped at the cycle of the last sample handed out, so the core
    // applies it at the next sample to be mixed.
    void setCycleForWrites() { emu.m_cpu->m_regs.cycle = delivered ? (delivered - 1) * kCyclesPerSample : 0; }

    void open() {
        delivered = 0;
        emu.m_cpu->m_regs.cycle = 0;
        spu->openSynchronous();
        // SPUCNT: enabled, unmuted. Main volume at max.
        setCycleForWrites();
        spu->writeRegister(H_SPUctrl, 0xc000);
        spu->writeRegister(0x0d80, 0x3fff);
        spu->writeRegister(0x0d82, 0x3fff);
    }

    void close() { spu->close(); }
};

PS1SPUBridge::PS1SPUBridge() : p(new Impl) {
    Impl::Use use(p);
    p->spu = new impl();
    p->spu->init();
    p->open();
}

PS1SPUBridge::~PS1SPUBridge() {
    {
        Impl::Use use(p);
        p->close();
        delete p->spu;
    }
    delete p;
}

void PS1SPUBridge::reset() {
    Impl::Use use(p);
    p->close();
    p->spu->init();
    p->open();
}

void PS1SPUBridge::writeReg(unsigned int reg, unsigned short val) {
    Impl::Use use(p);
    p->setCycleForWrites();
    p->spu->writeRegister(reg & 0xfff, val);
}

unsigned short PS1SPUBridge::readReg(unsigned int reg) {
    Impl::Use use(p);
    p->setCycleForWrites();
    return p->spu->readRegister(reg & 0xfff);
}

void PS1SPUBridge::writeRAM(unsigned int addr, const unsigned char* data, size_t len) {
    if (addr >= RAM_SIZE) return;
    len = std::min<size_t>(len, RAM_SIZE - addr) & ~(size_t)1;
    std::vector<uint16_t> words(len / 2);
    memcpy(words.data(), data, len);  // SPU RAM is little-endian, as is every host we build for
    Impl::Use use(p);
    p->setCycleForWrites();
    p->spu->writeRegister(H_SPUaddr, (uint16_t)(addr >> 3));
    p->spu->writeDMAMem(words.data(), (int)words.size());
}

void PS1SPUBridge::render(short* outL, short* outR, size_t total) {
    Impl::Use use(p);
    size_t done = 0;
    while (done < total) {
        const size_t n = std::min(kMaxRenderChunk, total - done);
        p->spu->mixSynchronous(p->scratch, n);
        for (size_t i = 0; i < n; i++) {
            if (outL) outL[done + i] = p->scratch[i * 2];
            if (outR) outR[done + i] = p->scratch[i * 2 + 1];
        }
        p->delivered += n;
        done += n;
    }
}

size_t PS1SPUBridge::encodedSize(size_t len) { return ((len + 27) / 28 + 1) * 16; }

size_t PS1SPUBridge::decodeADPCM(const unsigned char* in, size_t len, short* out) {
    PCSX::ADPCM::Decoder dec;
    dec.reset();
    const size_t blocks = len / 16;
    for (size_t b = 0; b < blocks; b++) {
        dec.decodeSPUBlock(in + b * 16, out + b * 28);
    }
    return blocks * 28;
}

size_t PS1SPUBridge::encodeADPCM(const short* in, size_t len, int loopStart, int loopEnd, unsigned char* out,
                                 size_t outCap) {
    using BA = PCSX::ADPCM::Encoder::BlockAttribute;
    const bool loop = loopStart >= 0 && loopEnd > loopStart && (size_t)loopStart < len;
    size_t blocks;
    size_t loopBlock = 0;
    if (loop) {
        loopBlock = (size_t)loopStart / 28;
        blocks = std::min<size_t>(((size_t)loopEnd + 27) / 28, (len + 27) / 28);
        if (blocks <= loopBlock) blocks = loopBlock + 1;
    } else {
        blocks = (len + 27) / 28;
        if (blocks == 0) blocks = 1;
    }
    const size_t size = (blocks + (loop ? 0 : 1)) * 16;
    if (size > outCap) return 0;

    PCSX::ADPCM::Encoder enc;
    enc.reset();
    int16_t block[28];
    for (size_t b = 0; b < blocks; b++) {
        for (size_t i = 0; i < 28; i++) {
            const size_t pos = b * 28 + i;
            block[i] = pos < len ? in[pos] : 0;
        }
        BA attr;
        if (loop) {
            if (b == blocks - 1) {
                attr = BA::LoopEnd;
            } else if (b == loopBlock) {
                attr = BA::LoopStart;
            } else if (b > loopBlock) {
                attr = BA::LoopBody;
            } else {
                attr = BA::OneShot;
            }
        } else {
            attr = (b == blocks - 1) ? BA::OneShotEnd : BA::OneShot;
        }
        enc.processSPUBlock(block, out + b * 16, attr);
    }
    if (!loop) enc.finishSPU(out + blocks * 16);
    return size;
}
