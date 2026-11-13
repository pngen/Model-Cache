#include "model_cache/detail/platform.hpp"

#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <filesystem>
namespace fs = std::filesystem;
#else
#include <filesystem>
#include <unistd.h>
#include <fcntl.h>
namespace fs = std::filesystem;
#endif

namespace model_cache {
namespace platform {

std::uint64_t now_millis() noexcept {
  using namespace std::chrono;
  return static_cast<std::uint64_t>(
      duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

bool path_exists(const std::string& p) noexcept {
  std::error_code ec;
  return fs::exists(p, ec);
}

bool is_directory(const std::string& p) noexcept {
  std::error_code ec;
  return fs::is_directory(p, ec);
}

void ensure_directory(const std::string& p) {
  if (p.empty()) fail(ErrorCode::invalid_argument, "ensure_directory: empty path");
  std::error_code ec;
  if (fs::create_directories(p, ec)) return;
  if (path_exists(p) && is_directory(p)) return;
  fail(ErrorCode::io_failure, "ensure_directory failed for '" + p + "': " + ec.message());
}

void remove_file_if_exists(const std::string& p) noexcept {
  std::error_code ec;
  fs::remove(p, ec);
}

void remove_directory_recursive(const std::string& p) noexcept {
  std::error_code ec;
  if (fs::exists(p, ec)) fs::remove_all(p, ec);
}

std::uint64_t file_size(const std::string& p) {
  std::error_code ec;
  auto sz = fs::file_size(p, ec);
  if (ec) fail(ErrorCode::not_found, "file_size '" + p + "': " + ec.message());
  return static_cast<std::uint64_t>(sz);
}

std::vector<std::uint8_t> read_file(const std::string& p, std::size_t max_bytes) {
  std::ifstream in(p, std::ios::binary | std::ios::ate);
  if (!in) fail(ErrorCode::not_found, "read_file cannot open '" + p + "'");
  const auto sz = static_cast<std::size_t>(in.tellg());
  if (sz > max_bytes) fail(ErrorCode::io_failure, "read_file exceeds max_bytes");
  std::vector<std::uint8_t> buf(sz);
  in.seekg(0, std::ios::beg);
  if (sz > 0) in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(sz));
  if (!in && sz > 0) fail(ErrorCode::io_failure, "read_file short read '" + p + "'");
  return buf;
}

Digest hash_file(const std::string& p) {
  std::ifstream in(p, std::ios::binary);
  if (!in) fail(ErrorCode::not_found, "hash_file cannot open '" + p + "'");
  DigestHasher h;
  std::vector<char> buf(1 << 20);
  while (in) {
    in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    const auto n = static_cast<std::size_t>(in.gcount());
    if (n > 0) h.update(buf.data(), n);
  }
  return h.finish();
}

#ifdef _WIN32
namespace {
std::wstring utf8_to_wide(const std::string& s) {
  if (s.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
  std::wstring w(static_cast<std::size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &w[0], n);
  return w;
}
std::string wide_to_utf8(const std::wstring& w) {
  if (w.empty()) return {};
  const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
  std::string s(static_cast<std::size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), &s[0], n, nullptr, nullptr);
  return s;
}
}  // namespace
#endif

void write_file_atomic(const std::string& path, const void* data, std::size_t bytes) {
  if (path.empty()) fail(ErrorCode::invalid_argument, "write_file_atomic: empty path");
#ifdef _WIN32
  // Unique per call within the process and spread across processes; an atomic
  // counter is safe from any thread. The suffix is no longer than it used to be.
  static std::atomic<std::uint32_t> staged{0};
  const std::uint32_t tag = (static_cast<std::uint32_t>(GetCurrentProcessId()) * 2654435761u) ^
                            staged.fetch_add(1, std::memory_order_relaxed);
  const std::string tmp = path + ".tmp" + std::to_string(now_millis()) + "." + std::to_string(tag);
  HANDLE h = CreateFileW(utf8_to_wide(tmp).c_str(), GENERIC_WRITE, 0, nullptr,
                         CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
  if (h == INVALID_HANDLE_VALUE) fail(ErrorCode::io_failure, "write_file_atomic: cannot create temp for '" + path + "'");
  bool okWrite = true;
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::size_t off = 0;
  while (off < bytes) {
    DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes - off, 16 * 1024 * 1024));
    DWORD written = 0;
    if (!WriteFile(h, p + off, chunk, &written, nullptr) || written != chunk) { okWrite = false; break; }
    off += chunk;
  }
  if (okWrite && !FlushFileBuffers(h)) okWrite = false;
  CloseHandle(h);
  if (!okWrite) {
    DeleteFileW(utf8_to_wide(tmp).c_str());
    fail(ErrorCode::io_failure, "write_file_atomic: write/flush failed for '" + path + "'");
  }
  if (!MoveFileExW(utf8_to_wide(tmp).c_str(), utf8_to_wide(path).c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(utf8_to_wide(tmp).c_str());
    fail(ErrorCode::io_failure, "write_file_atomic: commit rename failed for '" + path + "'");
  }
#else
  const std::string tmp = path + ".tmp." + std::to_string(now_millis());
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) fail(ErrorCode::io_failure, "write_file_atomic: temp open failed");
    out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
    out.flush();
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    std::remove(tmp.c_str());
    fail(ErrorCode::io_failure, "write_file_atomic: rename failed");
  }
#endif
}

void write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& data) {
  write_file_atomic(path, data.data(), data.size());
}

void fsync_file(const std::string& path) {
#ifdef _WIN32
  HANDLE h = CreateFileW(utf8_to_wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ,
                         nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (h != INVALID_HANDLE_VALUE) {
    FlushFileBuffers(h);
    CloseHandle(h);
  }
#else
  int fd = ::open(path.c_str(), O_RDONLY);
  if (fd >= 0) { ::fsync(fd); ::close(fd); }
#endif
}

std::vector<std::string> list_directory(const std::string& path) {
  std::vector<std::string> out;
  std::error_code ec;
  for (auto it = fs::directory_iterator(path, ec); it != fs::end(it); it.increment(ec)) {
    if (ec) break;
    out.push_back(it->path().filename().string());
  }
  return out;
}

bool is_safe_relative_path(std::string_view rel) {
  if (rel.empty()) return false;
  if (rel[0] == '/' || rel[0] == '\\') return false;
  if (rel.size() >= 2 && rel[1] == ':') return false;
  if (rel.find("..") != std::string_view::npos) return false;
  return rel.find('\0') == std::string_view::npos;
}

}  // namespace platform
}  // namespace model_cache
