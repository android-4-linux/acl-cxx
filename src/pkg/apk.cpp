// SPDX-License-Identifier: Apache-2.0
// Provenance: specs/pkg.md (pending), PKWARE .ZIP APPNOTE, zlib manual. No
// GPL-3.0 sources.

#include "pkg/apk.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <set>
#include <utility>

#include <zlib.h>

namespace acl::pkg {
namespace {

// Zip signatures (little-endian on disk).
constexpr std::uint32_t kEocdSig = 0x06054b50;     // end of central directory
constexpr std::uint32_t kCentralSig = 0x02014b50;  // central directory header
constexpr std::uint32_t kLocalSig = 0x04034b50;    // local file header
constexpr std::uint32_t kZip64LocatorSig = 0x07064b50;
constexpr std::size_t kEocdMinSize = 22;
constexpr std::size_t kMaxCommentSize = 0xffff;
constexpr std::uint16_t kMethodStore = 0;
constexpr std::uint16_t kMethodDeflate = 8;
constexpr std::uint32_t kZip64Sentinel = 0xffffffffu;

// Portable little-endian reads (do not assume host endianness or alignment).
std::uint16_t rd16(const std::uint8_t* p) noexcept {
  return static_cast<std::uint16_t>(p[0]) | static_cast<std::uint16_t>(p[1])
                                                << 8;
}
std::uint32_t rd32(const std::uint8_t* p) noexcept {
  return static_cast<std::uint32_t>(p[0]) |
         static_cast<std::uint32_t>(p[1]) << 8 |
         static_cast<std::uint32_t>(p[2]) << 16 |
         static_cast<std::uint32_t>(p[3]) << 24;
}

ZipError err(ZipStatus s, std::string msg) {
  return {s, std::move(msg)};
}

}  // namespace

const char* zip_status_name(ZipStatus status) noexcept {
  switch (status) {
    case ZipStatus::Ok:
      return "ok";
    case ZipStatus::Io:
      return "io";
    case ZipStatus::BadArchive:
      return "bad-archive";
    case ZipStatus::Unsupported:
      return "unsupported";
    case ZipStatus::NotFound:
      return "not-found";
    case ZipStatus::Inflate:
      return "inflate";
  }
  return "?";
}

void Apk::reset() noexcept {
  if (base_ != nullptr && base_ != MAP_FAILED) {
    ::munmap(const_cast<std::uint8_t*>(base_), size_);
  }
  if (fd_ >= 0)
    ::close(fd_);
  base_ = nullptr;
  size_ = 0;
  fd_ = -1;
  entries_.clear();
  layout_ = {};
  path_.clear();
}

Apk::~Apk() {
  reset();
}

Apk::Apk(Apk&& o) noexcept
    : path_(std::move(o.path_)),
      fd_(o.fd_),
      base_(o.base_),
      size_(o.size_),
      entries_(std::move(o.entries_)),
      layout_(o.layout_) {
  o.fd_ = -1;
  o.base_ = nullptr;
  o.size_ = 0;
}

Apk& Apk::operator=(Apk&& o) noexcept {
  if (this != &o) {
    reset();
    path_ = std::move(o.path_);
    fd_ = o.fd_;
    base_ = o.base_;
    size_ = o.size_;
    entries_ = std::move(o.entries_);
    layout_ = o.layout_;
    o.fd_ = -1;
    o.base_ = nullptr;
    o.size_ = 0;
  }
  return *this;
}

std::optional<Apk> Apk::open(const std::string& path, ZipError& out_error) {
  Apk apk;
  apk.path_ = path;
  apk.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (apk.fd_ < 0) {
    out_error =
        err(ZipStatus::Io, "open('" + path + "'): " + std::strerror(errno));
    return std::nullopt;
  }
  struct stat st{};
  if (::fstat(apk.fd_, &st) != 0 || st.st_size <= 0) {
    out_error = err(ZipStatus::Io, "fstat('" + path + "') failed or empty");
    return std::nullopt;
  }
  apk.size_ = static_cast<std::size_t>(st.st_size);
  void* m = ::mmap(nullptr, apk.size_, PROT_READ, MAP_PRIVATE, apk.fd_, 0);
  if (m == MAP_FAILED) {
    out_error =
        err(ZipStatus::Io, "mmap('" + path + "'): " + std::strerror(errno));
    return std::nullopt;
  }
  apk.base_ = static_cast<const std::uint8_t*>(m);

  if (!apk.index_central_directory(out_error))
    return std::nullopt;
  out_error = {};
  return apk;
}

bool Apk::index_central_directory(ZipError& out_error) {
  if (size_ < kEocdMinSize) {
    out_error = err(ZipStatus::BadArchive, "file smaller than EOCD record");
    return false;
  }
  // Scan backwards for the EOCD signature within the max comment window.
  const std::size_t scan = std::min(size_, kEocdMinSize + kMaxCommentSize);
  const std::uint8_t* end = base_ + size_;
  const std::uint8_t* eocd = nullptr;
  for (std::size_t back = kEocdMinSize; back <= scan; ++back) {
    const std::uint8_t* p = end - back;
    if (rd32(p) == kEocdSig) {
      eocd = p;
      break;
    }
  }
  if (eocd == nullptr) {
    out_error =
        err(ZipStatus::BadArchive, "end-of-central-directory not found");
    return false;
  }
  // Reject zip64: either an EOCD64 locator precedes the EOCD, or the EOCD
  // counts are the zip64 sentinel. libziparchive handles zip64; this
  // placeholder does not.
  const std::uint16_t total = rd16(eocd + 10);
  const std::uint32_t cd_size = rd32(eocd + 12);
  const std::uint32_t cd_offset = rd32(eocd + 16);
  if (total == 0xffff || cd_size == kZip64Sentinel ||
      cd_offset == kZip64Sentinel) {
    out_error = err(ZipStatus::Unsupported, "zip64 archive not supported yet");
    return false;
  }
  if (eocd - base_ >= 20 && rd32(eocd - 20) == kZip64LocatorSig) {
    out_error = err(ZipStatus::Unsupported, "zip64 archive not supported yet");
    return false;
  }
  if (static_cast<std::uint64_t>(cd_offset) + cd_size > size_) {
    out_error = err(ZipStatus::BadArchive, "central directory out of bounds");
    return false;
  }
  layout_.cd_offset = cd_offset;
  layout_.cd_size = cd_size;
  layout_.eocd_offset = static_cast<std::uint64_t>(eocd - base_);

  entries_.reserve(total);
  const std::uint8_t* p = base_ + cd_offset;
  const std::uint8_t* cd_end = p + cd_size;
  for (std::uint16_t n = 0; n < total; ++n) {
    if (p + 46 > cd_end || rd32(p) != kCentralSig) {
      out_error =
          err(ZipStatus::BadArchive, "malformed central directory header");
      return false;
    }
    Entry e;
    e.method = rd16(p + 10);
    e.crc32 = rd32(p + 16);
    e.comp_size = rd32(p + 20);
    e.uncomp_size = rd32(p + 24);
    const std::uint16_t name_len = rd16(p + 28);
    const std::uint16_t extra_len = rd16(p + 30);
    const std::uint16_t comment_len = rd16(p + 32);
    e.local_header_offset = rd32(p + 42);
    if (e.comp_size == kZip64Sentinel || e.uncomp_size == kZip64Sentinel ||
        e.local_header_offset == kZip64Sentinel) {
      out_error = err(ZipStatus::Unsupported, "zip64 entry not supported yet");
      return false;
    }
    const std::uint8_t* name = p + 46;
    if (name + name_len > cd_end) {
      out_error =
          err(ZipStatus::BadArchive, "central directory name out of bounds");
      return false;
    }
    e.name.assign(reinterpret_cast<const char*>(name), name_len);
    entries_.push_back(std::move(e));
    p = name + name_len + extra_len + comment_len;
  }
  return true;
}

const Apk::Entry* Apk::find(std::string_view name) const noexcept {
  for (const Entry& e : entries_) {
    if (e.name == name)
      return &e;
  }
  return nullptr;
}

bool Apk::data_offset(const Entry& entry,
                      std::uint64_t& out_offset,
                      ZipError& out_error) const {
  const std::uint64_t lho = entry.local_header_offset;
  if (lho + 30 > size_) {
    out_error = err(ZipStatus::BadArchive, "local header out of bounds");
    return false;
  }
  const std::uint8_t* p = base_ + lho;
  if (rd32(p) != kLocalSig) {
    out_error = err(ZipStatus::BadArchive, "bad local header signature");
    return false;
  }
  const std::uint16_t name_len = rd16(p + 26);
  const std::uint16_t extra_len = rd16(p + 28);
  const std::uint64_t off = lho + 30 + name_len + extra_len;
  if (off + entry.comp_size > size_) {
    out_error = err(ZipStatus::BadArchive, "entry data out of bounds");
    return false;
  }
  out_offset = off;
  return true;
}

bool Apk::read(const Entry& entry,
               std::vector<std::uint8_t>& out,
               ZipError& out_error) const {
  std::uint64_t off = 0;
  if (!data_offset(entry, off, out_error))
    return false;
  const std::uint8_t* src = base_ + off;

  if (entry.method == kMethodStore) {
    if (entry.comp_size != entry.uncomp_size) {
      out_error = err(ZipStatus::BadArchive, "stored size mismatch");
      return false;
    }
    out.assign(src, src + entry.comp_size);
    out_error = {};
    return true;
  }
  if (entry.method != kMethodDeflate) {
    out_error = err(ZipStatus::Unsupported, "compression method " +
                                                std::to_string(entry.method) +
                                                " not supported");
    return false;
  }

  out.resize(entry.uncomp_size);
  if (entry.uncomp_size == 0) {
    out_error = {};
    return true;
  }

  z_stream zs{};
  // Raw DEFLATE stream (no zlib/gzip header): windowBits = -15.
  if (inflateInit2(&zs, -15) != Z_OK) {
    out_error = err(ZipStatus::Inflate, "inflateInit2 failed");
    return false;
  }
  zs.next_in = const_cast<Bytef*>(src);
  zs.avail_in = static_cast<uInt>(entry.comp_size);
  zs.next_out = out.data();
  zs.avail_out = static_cast<uInt>(out.size());
  const int rc = inflate(&zs, Z_FINISH);
  const uLong produced = zs.total_out;
  inflateEnd(&zs);
  if (rc != Z_STREAM_END || produced != entry.uncomp_size) {
    out_error = err(
        ZipStatus::Inflate,
        "inflate('" + entry.name + "') failed (rc=" + std::to_string(rc) + ")");
    return false;
  }
  out_error = {};
  return true;
}

std::optional<std::vector<std::uint8_t>> Apk::read(std::string_view name,
                                                   ZipError& out_error) const {
  const Entry* e = find(name);
  if (e == nullptr) {
    out_error =
        err(ZipStatus::NotFound, "no entry '" + std::string(name) + "'");
    return std::nullopt;
  }
  std::vector<std::uint8_t> out;
  if (!read(*e, out, out_error))
    return std::nullopt;
  return out;
}

std::vector<std::string> Apk::native_abis() const {
  std::set<std::string> abis;
  constexpr std::string_view prefix = "lib/";
  for (const Entry& e : entries_) {
    if (e.name.size() <= prefix.size() ||
        e.name.compare(0, prefix.size(), prefix) != 0) {
      continue;
    }
    const std::size_t slash = e.name.find('/', prefix.size());
    if (slash == std::string::npos)
      continue;
    abis.insert(e.name.substr(prefix.size(), slash - prefix.size()));
  }
  return {abis.begin(), abis.end()};
}

std::vector<std::string> Apk::host_abi_candidates() {
#if defined(__x86_64__)
  return {"x86_64", "x86"};
#elif defined(__aarch64__)
  return {"arm64-v8a", "armeabi-v7a", "armeabi"};
#elif defined(__i386__)
  return {"x86"};
#elif defined(__arm__)
  return {"armeabi-v7a", "armeabi"};
#else
  return {};
#endif
}

std::string Apk::select_abi(const std::vector<std::string>& apk_abis) {
  for (const std::string& cand : host_abi_candidates()) {
    for (const std::string& a : apk_abis) {
      if (a == cand)
        return cand;
    }
  }
  return {};
}

}  // namespace acl::pkg
