// Stands in for pcsx-redux spu/sdlaudio.h: SDLAudio sink, the used methods as no-ops,
// except feedStreamData, which hands the mixed frames to a host-owned sink.
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
    // The host side of the sink. feedStreamData runs on the SPU mixer thread.
    struct Sink {
        virtual ~Sink() {}
        virtual void feed(const Frame *frames, size_t count) = 0;
    };
    // There is no way to reach the SPU's private SDLAudio from outside, so the host sets this
    // just before constructing the SPU and every SDLAudio constructed on that thread latches it.
    static inline thread_local Sink *s_nextSink = nullptr;
    SDLAudio(SettingsType &) : m_sink(s_nextSink) {}
    void setLua(Lua) {}
    uint32_t getFrameCount() { return 0; }
    uint32_t getPeriodFrames() { return 0; }
    size_t dropVoiceFrames(size_t) { return 0; }
    bool feedStreamData(const Frame *frames, size_t count, unsigned = 0) {
        if (m_sink) m_sink->feed(frames, count);
        return true;
    }
    size_t getFramesBuffered(unsigned = 0) { return 0; }
    uint32_t getCurrentFrames() { return 0; }
    void waitForGoal(uint32_t) {}

  private:
    Sink *m_sink;
};
}  // namespace SPU
}  // namespace PCSX
