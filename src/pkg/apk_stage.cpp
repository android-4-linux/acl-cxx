// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation; see apk_stage.h. POSIX file
// interfaces as documented in open(2), flock(2), rename(2). No GPL-3.0 sources.

#include "pkg/apk_stage.h"

#include "acl_config.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <set>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

#if ACL_HAVE_LIBCRYPTO
#include <openssl/evp.h>
#endif

namespace acl::pkg {
namespace {

namespace fs = std::filesystem;

// Unused verified copies kept besides the one in use. A rebuild-and-run loop
// makes a new copy per build; this bounds what it leaves behind.
constexpr std::size_t kKeepUnused = 3;

std::string errno_text(const std::string& what, const std::string& path) {
  return what + " " + path + ": " + std::strerror(errno);
}

// A real directory, owned by this user, not writable by group or others.
// lstat, so a symlink planted where the directory should be is refused.
bool is_private_dir(const std::string& path, std::string& error) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) {
    error = errno_text("cannot stat", path);
    return false;
  }
  if (!S_ISDIR(st.st_mode)) {
    error = path + " is not a directory";
    return false;
  }
  if (st.st_uid != ::geteuid()) {
    error = path + " is owned by another user";
    return false;
  }
  if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0) {
    error = path + " is writable by group or others";
    return false;
  }
  return true;
}

bool make_private_dir(const std::string& path, std::string& error) {
  if (::mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    error = errno_text("cannot create", path);
    return false;
  }
  return is_private_dir(path, error);
}

bool write_all(int fd, std::span<const std::uint8_t> data) noexcept {
  while (!data.empty()) {
    const ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    data = data.subspan(static_cast<std::size_t>(n));
  }
  return true;
}

// Write `data` to `dir/name` so that the name only ever refers to a complete
// file: a temporary name, then rename(2).
bool write_file_atomic(const std::string& dir,
                       const std::string& name,
                       std::span<const std::uint8_t> data,
                       std::string& error) {
  const std::string tmp =
      dir + "/." + name + "." + std::to_string(::getpid()) + ".tmp";
  const std::string path = dir + "/" + name;
  ::unlink(tmp.c_str());  // a leftover from an earlier process with this pid
  const int fd = ::open(
      tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0400);
  if (fd < 0) {
    error = errno_text("cannot create", tmp);
    return false;
  }
  const bool wrote = write_all(fd, data);
  const bool closed = ::close(fd) == 0;
  if (!wrote || !closed || ::rename(tmp.c_str(), path.c_str()) != 0) {
    error = errno_text("cannot write", path);
    ::unlink(tmp.c_str());
    return false;
  }
  return true;
}

}  // namespace

std::string private_cache_root(std::string& error) {
  std::string root;
  // An XDG path must be absolute; a relative one is ignored, as the spec says.
  if (const char* x = std::getenv("XDG_CACHE_HOME");
      x != nullptr && x[0] == '/') {
    root = std::string(x) + "/acl";
  } else if (const char* h = std::getenv("HOME"); h != nullptr && h[0] == '/') {
    root = std::string(h) + "/.cache/acl";
  } else {
    // No home. /tmp is shared, so the name carries the uid and the ownership
    // check below is what makes it safe to use.
    root = "/tmp/acl-" + std::to_string(::geteuid());
  }
  std::error_code ec;
  fs::create_directories(fs::path(root).parent_path(), ec);
  if (!make_private_dir(root, error))
    return {};
  return root;
}

std::string safe_package_dir(std::string_view package) {
  static constexpr std::string_view kFallback = "app";
  // A package name is dot-separated identifiers. Requiring a dot also keeps it
  // from colliding with a4l's own directory names, which have none.
  if (package.empty() || package.size() > 255 || package.front() == '.' ||
      package.back() == '.' || package.find('.') == std::string_view::npos ||
      package.find("..") != std::string_view::npos) {
    return std::string(kFallback);
  }
  const bool ok = std::ranges::all_of(package, [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '.';
  });
  return ok ? std::string(package) : std::string(kFallback);
}

bool extract_native_libs(const Apk& apk,
                         std::string_view abi,
                         const std::string& lib_dir,
                         int& count,
                         std::string& error) {
  count = 0;
  // lib_dir and its parent are a4l's; anything above them already exists.
  if (!make_private_dir(fs::path(lib_dir).parent_path().string(), error) ||
      !make_private_dir(lib_dir, error)) {
    return false;
  }
  const std::string want = "lib/" + std::string(abi) + "/";
  std::set<std::string> expected;
  for (const Apk::Entry& e : apk.entries()) {
    if (e.name.size() <= want.size() + 3 || !e.name.starts_with(want) ||
        !e.name.ends_with(".so")) {
      continue;
    }
    const std::string name = e.name.substr(want.size());
    if (name.find('/') != std::string::npos || name.front() == '.')
      continue;  // flat, and never a name that could hide or climb
    ZipError zerr;
    std::vector<std::uint8_t> bytes;
    if (!apk.read(e, bytes, zerr)) {
      error = "cannot read " + e.name + ": " + zerr.message;
      return false;
    }
    if (!write_file_atomic(lib_dir, name, bytes, error))
      return false;
    if (expected.insert(name).second)
      ++count;
  }
  // Remove what this APK did not put there. Another run's temporary files are
  // left alone; they are dot-names, which no library lookup resolves.
  std::error_code ec;
  for (const fs::directory_entry& d : fs::directory_iterator(lib_dir, ec)) {
    const std::string name = d.path().filename().string();
    if (!name.starts_with('.') && !expected.contains(name))
      fs::remove_all(d.path(), ec);
  }
  return true;
}

#if !ACL_HAVE_LIBCRYPTO

StageResult stage_verified(const Apk& source,
                           const std::string& /*cache_root*/,
                           int platform_sdk) {
  StageResult res;
  res.sig = verify_signature(source, platform_sdk);  // Unavailable
  return res;
}

#else  // ACL_HAVE_LIBCRYPTO

namespace {

std::string sha256_hex(std::span<const std::uint8_t> data) {
  std::uint8_t md[32];
  unsigned int len = 0;
  if (EVP_Digest(data.data(), data.size(), md, &len, EVP_sha256(), nullptr) !=
          1 ||
      len != sizeof(md)) {
    return {};
  }
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string out;
  out.reserve(64);
  for (const std::uint8_t v : md) {
    out.push_back(kDigits[v >> 4]);
    out.push_back(kDigits[v & 0xf]);
  }
  return out;
}

// Take a shared lock on `dir`, creating it. Returns -1 and fills `error` on
// failure. An evicting process holds the lock exclusively while it deletes, so
// after acquiring, a lock file with no links left means the directory went
// away while this waited, and the caller starts over.
int lock_dir(const std::string& dir, std::string& error) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    if (!make_private_dir(dir, error))
      return -1;
    const std::string lock = dir + "/.lock";
    const int fd =
        ::open(lock.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) {
      if (errno == ENOENT)
        continue;  // evicted between mkdir and open
      error = errno_text("cannot open", lock);
      return -1;
    }
    int rc = 0;
    do {
      rc = ::flock(fd, LOCK_SH);
    } while (rc != 0 && errno == EINTR);
    struct stat st{};
    if (rc == 0 && ::fstat(fd, &st) == 0 && st.st_nlink > 0)
      return fd;
    ::close(fd);
  }
  error = "cannot lock " + dir + " (evicted repeatedly)";
  return -1;
}

// Remove unused copies beyond the newest kKeepUnused. A copy is unused if its
// lock can be taken exclusively. Best effort: every failure is ignored.
void evict(const std::string& verified_root, const std::string& keep) {
  std::error_code ec;
  std::vector<std::pair<fs::file_time_type, fs::path>> others;
  for (const fs::directory_entry& d :
       fs::directory_iterator(verified_root, ec)) {
    if (d.path().filename() == keep || !d.is_directory(ec))
      continue;
    others.emplace_back(d.last_write_time(ec), d.path());
  }
  std::ranges::sort(others, std::greater{},
                    [](const auto& p) { return p.first; });
  for (std::size_t i = kKeepUnused; i < others.size(); ++i) {
    const fs::path& dir = others[i].second;
    const int fd =
        ::open((dir / ".lock").c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
      fs::remove_all(dir, ec);  // never finished being created
      continue;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0)
      fs::remove_all(dir, ec);
    ::close(fd);
  }
}

}  // namespace

StageResult stage_verified(const Apk& source,
                           const std::string& cache_root,
                           int platform_sdk) {
  StageResult res;
  // The signing block names the copy: it holds the signed digest of the rest
  // of the file, so a verified copy with this block is this APK. Without a
  // block there is nothing to stage; report what the source is.
  const std::span<const std::uint8_t> block = signing_block(source);
  const std::string key = block.empty() ? std::string{} : sha256_hex(block);
  if (key.empty()) {
    res.sig = verify_signature(source, platform_sdk);
    if (res.sig.verified()) {  // cannot happen: verifying needs a block
      res.sig = {};
      res.sig.status = SigStatus::Invalid;
      res.sig.message = "signing block could not be read";
    }
    return res;
  }

  const std::string verified_root = cache_root + "/verified";
  if (!make_private_dir(verified_root, res.error))
    return res;
  const std::string dir = verified_root + "/" + key;
  const int lock_fd = lock_dir(dir, res.error);
  if (lock_fd < 0)
    return res;
  const std::string path = dir + "/base.apk";

  // Open the copy and accept it only if it is this APK and it verifies.
  const auto open_verified = [&](SigResult& sig) -> std::optional<Apk> {
    ZipError zerr;
    std::optional<Apk> copy = Apk::open(path, zerr);
    sig = {};
    sig.status = SigStatus::Invalid;
    if (!copy) {
      sig.message = "verified copy is unreadable: " + zerr.message;
      return std::nullopt;
    }
    if (sha256_hex(signing_block(*copy)) != key) {
      sig.message = "APK changed while it was being copied";
      return std::nullopt;
    }
    sig = verify_signature(*copy, platform_sdk);
    if (!sig.verified())
      return std::nullopt;
    return copy;
  };

  std::error_code ec;
  std::optional<Apk> copy;
  bool reused = false;
  if (fs::exists(path, ec)) {
    copy = open_verified(res.sig);
    reused = copy.has_value();
    // A copy that no longer verifies was damaged or tampered with after it
    // was made. Discard it and make it again from the source.
  }
  if (!copy) {
    if (!write_file_atomic(dir, "base.apk", source.bytes(), res.error)) {
      ::close(lock_fd);
      return res;
    }
    copy = open_verified(res.sig);
  }
  if (!copy) {
    // The source does not verify. Leave nothing behind, including libraries
    // extracted from an earlier copy that did.
    fs::remove_all(dir, ec);
    ::close(lock_fd);
    return res;
  }

  ::utimensat(AT_FDCWD, dir.c_str(), nullptr, 0);  // mark as most recent
  evict(verified_root, key);

  res.staged.emplace();
  res.staged->apk = std::move(*copy);
  res.staged->dir = dir;
  res.staged->lib_dir = dir + "/lib";
  res.staged->reused = reused;
  res.staged->lock_fd = lock_fd;
  return res;
}

#endif  // ACL_HAVE_LIBCRYPTO

}  // namespace acl::pkg
