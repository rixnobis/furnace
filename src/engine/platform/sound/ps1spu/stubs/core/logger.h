// Stands in for pcsx-redux core/logger.h: logging hooks are no-ops.
#pragma once
namespace PCSX {
enum class LogClass { SPU };
template <LogClass C, bool Enabled>
struct Logger {
    template <typename... Args>
    static void Log(const char *, const Args &...) {}
};
typedef Logger<LogClass::SPU, false> PSXSPU_LOGGER;
}  // namespace PCSX
