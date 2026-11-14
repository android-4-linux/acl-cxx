// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "acl_config.h"
#include "pkg/apk.h"
#include "pkg/apk_stage.h"

using namespace acl::pkg;
namespace fs = std::filesystem;

namespace {

std::string env(const char* name) {
  const char* p = std::getenv(name);
  return p ? p : std::string{};
}

std::string fixture(const std::string& file) {
  return env("ACL_SIGNED_APKS_DIR") + "/" + file;
}

bool have_fixtures() {
  return !env("ACL_SIGNED_APKS_DIR").empty() &&
         fs::exists(fixture("ok-v3.apk"));
}

std::vector<char> slurp(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), {}};
}

// A fresh private directory, removed when the test ends.
struct TempDir {
  std::string path;
  TempDir() {
    std::string tmpl =
        (fs::temp_directory_path() / "a4l-stage-test-XXXXXX").string();
    REQUIRE(::mkdtemp(tmpl.data()) != nullptr);
    path = tmpl;
  }
  ~TempDir() {
    std::error_code ec;
    fs::permissions(path, fs::perms::owner_all, ec);
    fs::remove_all(path, ec);
  }
};

Apk open(const std::string& path) {
  ZipError err;
  auto apk = Apk::open(path, err);
  REQUIRE(apk.has_value());
  return std::move(*apk);
}

mode_t mode_of(const std::string& path) {
  struct stat st{};
  REQUIRE(::stat(path.c_str(), &st) == 0);
  return st.st_mode & 07777;
}

}  // namespace

TEST_CASE("a package name becomes a directory name only if it is one",
          "[pkg][stage]") {
  CHECK(safe_package_dir("com.example.app") == "com.example.app");
  CHECK(safe_package_dir("org.Foo_1.bar2") == "org.Foo_1.bar2");
  // Everything that could leave, hide in, or collide inside the cache.
  for (const char* bad :
       {"", "app", "verified", "../../etc", "com.example/../../x", "a..b",
        ".hidden.app", "com.example.", "com/example", "com.example\\x",
        "com.exa mple", "com.example\n", "/abs.olute"}) {
    INFO(bad);
    CHECK(safe_package_dir(bad) == "app");
  }
  CHECK(safe_package_dir(std::string(300, 'a') + ".b") == "app");
}

TEST_CASE("native libraries are replaced, not accumulated", "[pkg][stage]") {
  if (!have_fixtures()) {
    SKIP("signed-APK fixtures not built");
  }
  const TempDir tmp;
  const std::string lib_dir = tmp.path + "/com.example.app/lib";
  const Apk apk = open(fixture("ok-v3.apk"));
  int count = -1;
  std::string error;

  REQUIRE(extract_native_libs(apk, "x86_64", lib_dir, count, error));
  CHECK(count == 1);
  CHECK(fs::file_size(lib_dir + "/libfoo.so") == 11);
  CHECK(mode_of(lib_dir) == 0700);

  // A library from an earlier version, and one planted since, must not survive.
  std::ofstream(lib_dir + "/libstale.so") << "old";
  std::ofstream(lib_dir + "/libfoo.so", std::ios::trunc) << "tampered";
  REQUIRE(extract_native_libs(apk, "x86_64", lib_dir, count, error));
  CHECK_FALSE(fs::exists(lib_dir + "/libstale.so"));
  CHECK(fs::file_size(lib_dir + "/libfoo.so") == 11);

  // An ABI the APK does not carry extracts nothing and clears the directory.
  REQUIRE(extract_native_libs(apk, "riscv64", lib_dir, count, error));
  CHECK(count == 0);
  CHECK(fs::is_empty(lib_dir));

  // A directory someone else can write is refused, not used.
  REQUIRE(::chmod(lib_dir.c_str(), 0777) == 0);
  CHECK_FALSE(extract_native_libs(apk, "x86_64", lib_dir, count, error));
  CHECK(error.find("writable by group or others") != std::string::npos);
}

#if ACL_HAVE_LIBCRYPTO

TEST_CASE("an enforced APK runs from a private copy that verifies",
          "[pkg][stage]") {
  if (!have_fixtures()) {
    SKIP("signed-APK fixtures not built");
  }
  const TempDir tmp;
  const std::string source = fixture("ok-v3.apk");

  StageResult first = stage_verified(open(source), tmp.path);
  REQUIRE(first.error.empty());
  REQUIRE(first.staged.has_value());
  CHECK(first.sig.verified());
  CHECK_FALSE(first.staged->reused);
  const std::string copy = first.staged->dir + "/base.apk";
  CHECK(first.staged->apk.path() == copy);
  CHECK(copy.starts_with(tmp.path + "/verified/"));
  CHECK(slurp(copy) == slurp(source));
  CHECK(mode_of(copy) == 0400);
  CHECK(mode_of(first.staged->dir) == 0700);
  CHECK(first.staged->lock_fd >= 0);

  // A second run verifies the same copy again instead of copying.
  StageResult second = stage_verified(open(source), tmp.path);
  REQUIRE(second.staged.has_value());
  CHECK(second.staged->reused);
  CHECK(second.staged->dir == first.staged->dir);

  // A copy damaged after it was made is not trusted: it is remade.
  REQUIRE(::chmod(copy.c_str(), 0600) == 0);
  {
    std::fstream f(copy, std::ios::binary | std::ios::in | std::ios::out);
    f.seekp(40);
    f.put('X');
  }
  REQUIRE(slurp(copy) != slurp(source));
  StageResult third = stage_verified(open(source), tmp.path);
  REQUIRE(third.staged.has_value());
  CHECK_FALSE(third.staged->reused);
  CHECK(third.sig.verified());
  CHECK(slurp(copy) == slurp(source));
}

TEST_CASE("an APK that does not verify leaves no copy behind", "[pkg][stage]") {
  if (!have_fixtures()) {
    SKIP("signed-APK fixtures not built");
  }
  const struct {
    const char* file;
    SigStatus status;
  } cases[] = {
      {"unsigned.apk", SigStatus::Unsigned},
      {"unsigned-block-without-signature.apk", SigStatus::Unsigned},
      {"unsupported-verity-only.apk", SigStatus::Unsupported},
      {"bad-entry-byte.apk", SigStatus::Invalid},
      {"bad-signature.apk", SigStatus::Invalid},
      {"bad-block-sizes-differ.apk", SigStatus::Invalid},
      {"bad-trailing-data.apk", SigStatus::Invalid},
  };
  for (const auto& c : cases) {
    INFO(c.file);
    const TempDir tmp;
    const StageResult r = stage_verified(open(fixture(c.file)), tmp.path);
    CHECK_FALSE(r.staged.has_value());
    CHECK(r.error.empty());
    CHECK(r.sig.status == c.status);
    CHECK_FALSE(r.sig.message.empty());
    const std::string store = tmp.path + "/verified";
    CHECK((!fs::exists(store) || fs::is_empty(store)));
  }
}

TEST_CASE("the copy is found by what the source's signature covers",
          "[pkg][stage]") {
  if (!have_fixtures()) {
    SKIP("signed-APK fixtures not built");
  }
  const TempDir tmp;
  const std::string good = fixture("ok-v2-eocd-comment.apk");
  StageResult staged = stage_verified(open(good), tmp.path);
  REQUIRE(staged.staged.has_value());
  const std::string copy = staged.staged->dir + "/base.apk";

  // The same file with an entry byte changed keeps its signing block, so it
  // maps to the same copy. What runs is the copy, which still verifies; the
  // tampered bytes are never read.
  const std::string tampered_path = tmp.path + "/tampered.apk";
  std::vector<char> bytes = slurp(good);
  bytes[40] ^= 0x01;
  std::ofstream(tampered_path, std::ios::binary)
      .write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  StageResult again = stage_verified(open(tampered_path), tmp.path);
  REQUIRE(again.staged.has_value());
  CHECK(again.staged->reused);
  CHECK(again.sig.verified());
  CHECK(slurp(copy) == slurp(good));

  // With no earlier copy to fall back on, the same tampered file is refused.
  const TempDir empty;
  const StageResult cold = stage_verified(open(tampered_path), empty.path);
  CHECK_FALSE(cold.staged.has_value());
  CHECK(cold.sig.status == SigStatus::Invalid);

  // A different APK gets a different copy.
  StageResult other = stage_verified(open(fixture("ok-v3.apk")), tmp.path);
  REQUIRE(other.staged.has_value());
  CHECK(other.staged->dir != staged.staged->dir);
}

TEST_CASE("a cache someone else can write is refused", "[pkg][stage]") {
  if (!have_fixtures()) {
    SKIP("signed-APK fixtures not built");
  }
  const Apk apk = open(fixture("ok-v3.apk"));
  {
    const TempDir tmp;
    REQUIRE(::mkdir((tmp.path + "/verified").c_str(), 0777) == 0);
    REQUIRE(::chmod((tmp.path + "/verified").c_str(), 0777) == 0);
    const StageResult r = stage_verified(apk, tmp.path);
    CHECK_FALSE(r.staged.has_value());
    CHECK(r.error.find("writable by group or others") != std::string::npos);
  }
  {
    // A symlink where the store should be is not followed.
    const TempDir tmp;
    const TempDir elsewhere;
    fs::create_directory_symlink(elsewhere.path, tmp.path + "/verified");
    const StageResult r = stage_verified(apk, tmp.path);
    CHECK_FALSE(r.staged.has_value());
    CHECK(r.error.find("is not a directory") != std::string::npos);
    CHECK(fs::is_empty(elsewhere.path));
  }
}

TEST_CASE("unused copies are evicted and copies in use are not",
          "[pkg][stage]") {
  if (!have_fixtures()) {
    SKIP("signed-APK fixtures not built");
  }
  const TempDir tmp;
  const char* files[] = {
      "ok-v2-ec-sha256.apk",
      "ok-v2-rsa-pkcs1-sha256.apk",
      "ok-v2-dsa-sha256.apk",
      "ok-v2-two-signers.apk",
      "ok-v2-multi-chunk.apk",
      "ok-v2-padded-block.apk",
      "ok-v3.apk",
      "ok-v31-rotated.apk",
  };
  const auto count = [&] {
    std::size_t n = 0;
    for (const auto& d : fs::directory_iterator(tmp.path + "/verified")) {
      (void)d;
      ++n;
    }
    return n;
  };
  // The first copy stays locked, as a running app would hold it.
  StageResult held = stage_verified(open(fixture(files[0])), tmp.path);
  REQUIRE(held.staged.has_value());
  const std::string held_dir = held.staged->dir;
  for (std::size_t i = 1; i < std::size(files); ++i) {
    INFO(files[i]);
    StageResult r = stage_verified(open(fixture(files[i])), tmp.path);
    REQUIRE(r.staged.has_value());
    ::close(r.staged->lock_fd);  // this "run" has ended
    // The one just made, three unused ones, and the one still held.
    CHECK(count() <= 5);
    CHECK(fs::exists(held_dir + "/base.apk"));
  }
  CHECK(count() == 5);
}

#endif  // ACL_HAVE_LIBCRYPTO
