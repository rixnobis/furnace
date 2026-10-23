// Stands in for pcsx-redux support/file.h: IO<File>/PosixFile with only the WAV-dump surface; writes are dropped.
#pragma once
#include <stddef.h>
#include <sys/types.h>
#include <filesystem>
#include <memory>
#include <string_view>
namespace PCSX {
enum Truncate { TRUNCATE };
struct FileOps {
    static constexpr Truncate TRUNCATE = ::PCSX::TRUNCATE;
};
class File {
  public:
    virtual ~File() {}
    virtual ssize_t write(const void *, size_t size) { return size; }
    virtual bool failed() { return false; }
    void writeString(const std::string_view &str) { write(str.data(), str.size()); }
    template <typename T>
    void write(T val) { write(&val, sizeof(T)); }
    template <typename T>
    void writeAt(T, size_t) {}
};
class PosixFile : public File {
  public:
    PosixFile(const std::filesystem::path &, Truncate) {}
};
template <class T>
class IO {
  public:
    IO() = default;
    IO(T *p) : m_p(p) {}
    T *operator->() const { return m_p.get(); }
    explicit operator bool() const { return !!m_p; }
    void reset() { m_p.reset(); }

  private:
    std::shared_ptr<T> m_p;
};
}  // namespace PCSX
