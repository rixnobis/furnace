// Stands in for pcsx-redux core/r3000a.h: just the CPU cycle counter.
#pragma once
#include <stdint.h>
namespace PCSX {
struct R3000Acpu {
    struct { uint64_t cycle = 0; } m_regs;
};
}  // namespace PCSX
