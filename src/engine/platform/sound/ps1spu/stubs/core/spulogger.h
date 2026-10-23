// Stands in for pcsx-redux core/spulogger.h: log() is a no-op.
#pragma once
#include <stddef.h>
#include <stdint.h>
namespace PCSX {
class SPULogger {
  public:
    enum class Kind : uint8_t { DMAWrite, DMARead, PIOWrite, TransferAddr, VoiceStart, VoiceLoop, KeyOn, KeyOff, ReverbAddr };
    static constexpr uint8_t c_noVoice = 0xff;
    void log(Kind, uint32_t, uint32_t, uint32_t, uint8_t = c_noVoice) {}
};
}  // namespace PCSX
