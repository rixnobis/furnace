// Ours. Definitions for the SPU impl members whose upstream files are not vendored:
// freeze.cc (save states), debug.cc and cfg.cc (ImGui windows). None is used by Furnace.
#include "spu/interface.h"

void PCSX::SPU::impl::save(SaveStates::SPU &) {}
void PCSX::SPU::impl::load(const SaveStates::SPU &) {}
// Redux builds reset() on the save state machinery (freeze.cc); the bridge never calls it.
void PCSX::SPU::impl::reset() {}
void PCSX::SPU::impl::debug() {}
bool PCSX::SPU::impl::configure() { return false; }
