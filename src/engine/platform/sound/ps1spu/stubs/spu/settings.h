// Stands in for pcsx-redux spu/settings.h: only the setting types the vendored SPU reads.
#pragma once
#include "support/settings.h"
namespace PCSX {
namespace SPU {
typedef Setting<bool, TYPESTRING("Streaming"), true> Streaming;
typedef Setting<int, TYPESTRING("Volume"), 3> Volume;
typedef Setting<int, TYPESTRING("Interp"), 2> Interpolation;
typedef Setting<int, TYPESTRING("LatencyMargin"), 15> LatencyMargin;
typedef Settings<Streaming, Volume, Interpolation, LatencyMargin> SettingsType;
}  // namespace SPU
}  // namespace PCSX
