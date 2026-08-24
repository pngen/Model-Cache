#pragma once
// Model Cache — platform abstraction for durable filesystem persistence.
//
// Everything persists through this layer so there is exactly one place where
// atomic write, fsync (FlushFileBuffers on Windows), path safety, and streaming
// hash are implemented. All functions throw model_cache::Error on failure; they
// never silently fabricate success.

#include "model_cache/digest.hpp"
#include "model_cache/error.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace model_cache {
namespace platform {

/// Epoch milliseconds (UTC), for record timestamps.
std::uint64_t now_millis() noexcept;

/// True if a path exists (file or directory).
bool path_exists(const std::string& path) noexcept;

/// True if path is a directory.
bool is_directory(const std::string& path) noexcept;

/// Create a directory and all required parents. No-op if it already exists.
void ensure_directory(const std::string& path);

/// Remove a single file if present. Does not error if missing.
void remove_file_if_exists(const std::string& path) noexcept;

/// Recursively remove a directory tree. Does not error if missing.
void remove_directory_recursive(const std::string& path) noexcept;

/// Size of a file in bytes; throws ErrorCode::not_found if missing.
std::uint64_t file_size(const std::string& path);

/// Read an entire file into a byte vector (bounded by size).
std::vector<std::uint8_t> read_file(const std::string& path,
                                    std::size_t max_bytes = std::size_t(-1));

/// Stream hash a file with SHA-256 without loading it fully into memory.
Digest hash_file(const std::string& path);

/// Durable atomic write: temp-file staging + flush + atomic rename/commit.
/// On failure the prior file content is left untouched.
void write_file_atomic(const std::string& path, const void* data, std::size_t bytes);
void write_file_atomic(const std::string& path, const std::vector<std::uint8_t>& data);

/// Best-effort fsync of a file's contents to durable storage.
void fsync_file(const std::string& path);

/// Remove files matching a simple suffix under a directory (stale temp cleanup).
std::vector<std::string> list_directory(const std::string& path);

/// Validate that a relative path does not escape a root (no "..", no absolute
/// path, no drive/ UNC). Uses the platform separator.
bool is_safe_relative_path(std::string_view rel);

}  // namespace platform
}  // namespace model_cache
