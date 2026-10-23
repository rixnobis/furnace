// Stands in for pcsx-redux spu/sdlaudio.h: the SDLAudio methods the SPU calls, all no-ops.
// The bridge drives the mixer itself and reads the mixed frames straight out of the SPU's
// output buffer, so nothing is ever fed to a device (flushOutput is never reached).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "core/spu.h"
#include "spu/settings.h"
namespace PCSX {
namespace SPU {
class SDLAudio {
  public:
    struct Frame {
        int16_t L = 0, R = 0;
    };
    SDLAudio(SettingsType &) {}
    void setLua(Lua) {}
    uint32_t getFrameCount() { return 0; }
    uint32_t getPeriodFrames() { return 0; }
    size_t dropVoiceFrames(size_t) { return 0; }
    bool feedStreamData(const Frame *, size_t, unsigned = 0) { return true; }
    size_t getFramesBuffered(unsigned = 0) { return 0; }
    uint32_t getCurrentFrames() { return 0; }
    void waitForGoal(uint32_t) {}
};
}  // namespace SPU
}  // namespace PCSX
