// Stands in for pcsx-redux core/spu.h: SPUInterface (same virtuals) plus a no-op Lua handle.
#pragma once
#include <stdint.h>
#include <filesystem>
#include "core/decode_xa.h"
#include "core/psxemulator.h"
#include "core/r3000a.h"
#include "core/sstate.h"
#include "json.hpp"
#define LUA_GLOBALSINDEX (-10002)
struct Lua {  // stand-in for lua/luawrapper.h Lua: every call is a no-op
    void getfieldtable(const char *, int = 0) {}
    template <typename T>
    void push(T) {}
    void settable() {}
    void pop() {}
};
namespace PCSX {
class SPUInterface {
  public:
    using json = nlohmann::json;
    virtual ~SPUInterface() {}
    void interrupt() {}
    virtual bool open() = 0;
    virtual long init(void) = 0;
    virtual long shutdown(void) = 0;
    virtual long close(void) = 0;
    virtual uint16_t readRegister(uint32_t) = 0;
    virtual void writeRegister(uint32_t, uint16_t) = 0;
    virtual void playCDDAchannel(int16_t *, int) = 0;
    virtual void playADPCMchannel(xa_decode_t *) = 0;
    virtual void setXAVolume(const uint8_t atv[4]) = 0;
    virtual void writeDMAMem(uint16_t *, int) = 0;
    virtual void readDMAMem(uint16_t *, int) = 0;
    virtual void lockSPURAM() = 0;
    virtual void unlockSPURAM() = 0;
    virtual uint8_t *getSpuRam() = 0;
    static constexpr size_t c_spuRamSize = 512 * 1024;
    virtual uint32_t getTransferAddr() = 0;
    virtual void resetCaptureBuffer() = 0;
    virtual json getCfg() = 0;
    virtual void setCfg(const json &j) = 0;
    virtual void debug() = 0;
    virtual bool configure() = 0;
    virtual void save(SaveStates::SPU &) = 0;
    virtual void load(const SaveStates::SPU &) = 0;
    virtual uint32_t getCurrentFrames() = 0;
    virtual void waitForGoal(uint32_t goal) = 0;
    virtual uint32_t getLeadFrames() = 0;
    virtual void advanceTo(uint64_t cycle) = 0;
    virtual uint32_t getFrameCount() = 0;
    virtual void setLua(Lua L) = 0;
    virtual bool setWavDump(const std::filesystem::path &path) = 0;
    bool m_showDebug = false;
    bool m_showCfg = false;

  protected:
    void scheduleInterrupt() {}
};
}  // namespace PCSX
