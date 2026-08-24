#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace testfw {
namespace fs = std::filesystem;

inline std::uint64_t utime() {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

struct TempDir {
  std::string path;
  TempDir() {
    const auto base = fs::temp_directory_path() / ("mc_test_" + std::to_string(utime()) + "_" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    fs::create_directories(base);
    path = base.string();
  }
  ~TempDir() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
  std::string join(const std::string& p) const { return (fs::path(path) / p).string(); }
};
}  // namespace testfw
