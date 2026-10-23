// Stands in for pcsx-redux support/settings.h: Setting/Settings holding defaults, no persistence.
#pragma once
#include <tuple>
#include <type_traits>
#include "json.hpp"
#include "support/typestring-wrapper.h"
namespace PCSX {
template <typename T, typename Name, T def = T()>
struct Setting {
    T value = def;
    operator T() const { return value; }
};
template <typename... S>
class Settings {
  public:
    template <typename T>
    T &get() { return std::get<T>(m_s); }
    nlohmann::json serialize() const { return nlohmann::json::object(); }
    void deserialize(const nlohmann::json &) {}
    void reset() { m_s = std::tuple<S...>(); }
    template <typename L>
    void pushValue(L &) {}

  private:
    std::tuple<S...> m_s;
};
}  // namespace PCSX
