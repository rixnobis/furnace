// Stands in for pcsx-redux core/psxemulator.h: g_emulator/g_system with the few members the SPU touches.
// g_emulator is thread_local: each PS1SPUBridge owns an Emulator (its CPU cycle counter) and
// points this at it for the duration of each call into the core, so instances on different
// threads never see each other's counter.
#pragma once
#include <stdint.h>
#include <memory>
#include <string>
#include "core/logger.h"
#include "core/r3000a.h"
#include "core/spulogger.h"
namespace PCSX {
struct System {
    template <typename... Args>
    void log(LogClass, const char *, const Args &...) {}
};
struct Emulator {
    static constexpr uint32_t m_psxClockSpeed = 33868800;
    std::unique_ptr<R3000Acpu> m_cpu = std::make_unique<R3000Acpu>();
    std::unique_ptr<SPULogger> m_spuLogger = std::make_unique<SPULogger>();
};
inline thread_local Emulator *g_emulator = nullptr;
// stateless (log is a no-op), so one shared instance serves every bridge.
inline System s_system;
inline System *g_system = &s_system;
}  // namespace PCSX
