// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/pkg.md (pending), PKWARE .ZIP APPNOTE (zip format), AOSP
// libziparchive behavior docs. No GPL-3.0 sources.
#ifndef ACL_PKG_APK_H
#define ACL_PKG_APK_H

// A minimal read-only APK (zip) reader: enumerate central-directory entries and
// inflate them. This is a Phase 1 placeholder for AOSP `libziparchive`; the API
// is kept small so the swap is localized to pkg/. Standard zip only — zip64 and
// encrypted entries are rejected with a clear error.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace acl::pkg {

enum class ZipStatus {
  Ok,
  Io,           // open/mmap/read failure
  BadArchive,   // not a zip / corrupt central directory
  Unsupported,  // zip64, encryption, or an unknown compression method
  NotFound,     // no such entry
  Inflate,      // decompression failed
};

const char* zip_status_name(ZipStatus status) noexcept;

struct ZipError {
  ZipStatus status = ZipStatus::Ok;
  std::string message;
  bool is_ok() const noexcept { return status == ZipStatus::Ok; }
  explicit operator bool() const noexcept { return status != ZipStatus::Ok; }
};

class Apk {
 public:
  struct Entry {
    std::string name;
    std::uint16_t method = 0;  // 0 = stored, 8 = deflate
    std::uint32_t crc32 = 0;
    std::uint64_t comp_size = 0;
    std::uint64_t uncomp_size = 0;
    std::uint64_t local_header_offset = 0;
  };

  Apk() = default;
  ~Apk();
  Apk(Apk&&) noexcept;
  Apk& operator=(Apk&&) noexcept;
  Apk(const Apk&) = delete;
  Apk& operator=(const Apk&) = delete;

  // Open and index `path`. On failure returns nullopt and fills `out_error`.
  static std::optional<Apk> open(const std::string& path, ZipError& out_error);

  const std::vector<Entry>& entries() const noexcept { return entries_; }
  const Entry* find(std::string_view name) const noexcept;

  // Inflate `entry` into `out` (resized to the uncompressed size).
  bool read(const Entry& entry,
            std::vector<std::uint8_t>& out,
            ZipError& out_error) const;

  // Convenience: read a named entry, or nullopt if missing/failed.
  std::optional<std::vector<std::uint8_t>> read(std::string_view name,
                                                ZipError& out_error) const;

  // Distinct ABI directory names found under lib/ (e.g. "arm64-v8a"), sorted.
  std::vector<std::string> native_abis() const;

  const std::string& path() const noexcept { return path_; }

  // Where the zip structures this reader indexed sit in the file. The signature
  // verifier (apk_verify.h) hashes exactly these regions, so the bytes that are
  // verified and the bytes that are read cannot be two different parses.
  struct Layout {
    std::uint64_t cd_offset = 0;    // central directory start
    std::uint64_t cd_size = 0;      // central directory length
    std::uint64_t eocd_offset = 0;  // end-of-central-directory record start
  };
  const Layout& layout() const noexcept { return layout_; }
  // The whole mapped file. Valid for the lifetime of this Apk.
  std::span<const std::uint8_t> bytes() const noexcept {
    return {base_, size_};
  }

  // This build's supported ABIs in Android's preference order (primary first),
  // e.g. x86_64 -> {"x86_64","x86"}, arm64 ->
  // {"arm64-v8a","armeabi-v7a","armeabi"}.
  static std::vector<std::string> host_abi_candidates();
  // The ABI to load for `apk_abis` on this host: the first host candidate
  // present, else "". A non-empty result with no host match means the app's
  // native code can't run here.
  static std::string select_abi(const std::vector<std::string>& apk_abis);

 private:
  bool index_central_directory(ZipError& out_error);
  // Compute the file offset of an entry's compressed data by reading its local
  // header (whose name/extra lengths may differ from the central record).
  bool data_offset(const Entry& entry,
                   std::uint64_t& out_offset,
                   ZipError& out_error) const;

  std::string path_;
  int fd_ = -1;
  const std::uint8_t* base_ = nullptr;  // mmap'd, read-only
  std::size_t size_ = 0;
  std::vector<Entry> entries_;
  Layout layout_;

  void reset() noexcept;
};

}  // namespace acl::pkg

#endif  // ACL_PKG_APK_H
