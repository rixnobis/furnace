// Ours. See bridge.h. This TU is C++20, like the vendored core.
#include "bridge.h"

#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

#include "core/psxemulator.h"
#include "spu/interface.h"
#include "spu/registers.h"
#include "supportpsx/adpcm.h"

namespace {

// The core reads the CPU cycle counter through the g_emulator global, so every PS1SPUBridge
// shares one. Calls into the core are serialised by this lock, and each one sets the counter
// to its own instance's time first.
std::mutex s_coreLock;
PCSX::Emulator s_emulator;
PCSX::System s_system;

// The core's clock: 768 CPU cycles per 44.1 kHz sample.
constexpr uint64_t kCyclesPerSample = PCSX::Emulator::m_psxClockSpeed / 44100;
// Largest step published to the core at once; it resyncs on forward jumps over a second.
constexpr size_t kMaxRenderChunk = 4096;

}  // namespace

struct PS1SPUBridge::Impl final : PCSX::SPU::SDLAudio::Sink {
    PCSX::SPU::impl* spu = nullptr;
    // output samples handed to the caller so far
    uint64_t delivered = 0;

    std::mutex sinkLock;
    std::condition_variable sinkCond;
    std::deque<PCSX::SPU::SDLAudio::Frame> frames;
    uint64_t fed = 0;

    // Runs on the core's mixer thread.
    void feed(const PCSX::SPU::SDLAudio::Frame* f, size_t count) override {
        {
            std::lock_guard<std::mutex> lock(sinkLock);
            frames.insert(frames.end(), f, f + count);
            fed += count;
        }
        sinkCond.notify_all();
    }

    // A write made "now" is stamped at the cycle of the last sample handed out, so the core
    // applies it at the next sample to be mixed.
    void setCycleForWrites() {
        PCSX::g_emulator->m_cpu->m_regs.cycle = delivered ? (delivered - 1) * kCyclesPerSample : 0;
    }

    void open() {
        delivered = 0;
        {
            std::lock_guard<std::mutex> lock(sinkLock);
            frames.clear();
            fed = 0;
        }
        PCSX::g_emulator->m_cpu->m_regs.cycle = 0;
        spu->open();
        // SPUCNT: enabled, unmuted. Main volume at max.
        setCycleForWrites();
        spu->writeRegister(H_SPUctrl, 0xc000);
        spu->writeRegister(0x0d80, 0x3fff);
        spu->writeRegister(0x0d82, 0x3fff);
    }
};

PS1SPUBridge::PS1SPUBridge() : p(new Impl) {
    std::lock_guard<std::mutex> lock(s_coreLock);
    PCSX::g_emulator = &s_emulator;
    PCSX::g_system = &s_system;
    PCSX::SPU::SDLAudio::s_nextSink = p;
    p->spu = new PCSX::SPU::impl();
    PCSX::SPU::SDLAudio::s_nextSink = nullptr;
    p->spu->init();
    p->open();
}

PS1SPUBridge::~PS1SPUBridge() {
    {
        std::lock_guard<std::mutex> lock(s_coreLock);
        p->spu->close();
        delete p->spu;
    }
    delete p;
}

void PS1SPUBridge::reset() {
    std::lock_guard<std::mutex> lock(s_coreLock);
    p->spu->close();
    p->spu->init();
    p->open();
}

void PS1SPUBridge::writeReg(unsigned int reg, unsigned short val) {
    std::lock_guard<std::mutex> lock(s_coreLock);
    p->setCycleForWrites();
    p->spu->writeRegister(reg & 0xfff, val);
}

unsigned short PS1SPUBridge::readReg(unsigned int reg) {
    std::lock_guard<std::mutex> lock(s_coreLock);
    p->setCycleForWrites();
    return p->spu->readRegister(reg & 0xfff);
}

void PS1SPUBridge::writeRAM(unsigned int addr, const unsigned char* data, size_t len) {
    if (addr >= RAM_SIZE) return;
    len = std::min<size_t>(len, RAM_SIZE - addr) & ~(size_t)1;
    std::vector<uint16_t> words(len / 2);
    memcpy(words.data(), data, len);  // SPU RAM is little-endian, as is every host we build for
    std::lock_guard<std::mutex> lock(s_coreLock);
    p->setCycleForWrites();
    p->spu->writeRegister(H_SPUaddr, (uint16_t)(addr >> 3));
    p->spu->writeDMAMem(words.data(), (int)words.size());
}

void PS1SPUBridge::render(short* outL, short* outR, size_t total) {
    std::lock_guard<std::mutex> lock(s_coreLock);
    size_t done = 0;
    while (done < total) {
        const size_t n = std::min(kMaxRenderChunk, total - done);
        const uint64_t target = p->delivered + n;
        // Publishing cycle (target-1)*768 lets the core mix samples [0, target). Reading ENDX
        // makes the CPU side wait (catchUp) until the mixer is there.
        PCSX::g_emulator->m_cpu->m_regs.cycle = (target - 1) * kCyclesPerSample;
        p->spu->readRegister(H_SPUMute1);
        // The mixer publishes its progress just before handing the frames over, so they may
        // still be in flight.
        std::unique_lock<std::mutex> sl(p->sinkLock);
        p->sinkCond.wait(sl, [&]() { return p->fed >= target; });
        for (size_t i = 0; i < n; i++) {
            const auto& f = p->frames.front();
            if (outL) outL[done + i] = f.L;
            if (outR) outR[done + i] = f.R;
            p->frames.pop_front();
        }
        p->delivered = target;
        done += n;
    }
}

size_t PS1SPUBridge::encodedSize(size_t len) { return ((len + 27) / 28 + 1) * 16; }

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
