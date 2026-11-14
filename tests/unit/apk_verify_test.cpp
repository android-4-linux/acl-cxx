// SPDX-License-Identifier: Apache-2.0
// Provenance: original test. No GPL-3.0 sources.

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "acl_config.h"
#include "pkg/apk.h"
#include "pkg/apk_verify.h"

using namespace acl::pkg;

namespace {

// One line of the manifest make_signed_apks.py writes beside its APKs.
struct Expected {
  std::string file;
  std::string status;
  std::string scheme;
  std::vector<std::string> signers;
};

std::string env(const char* name) {
  const char* p = std::getenv(name);
  return p ? p : std::string{};
}

std::vector<Expected> load_manifest() {
  std::vector<Expected> out;
  std::ifstream in(env("ACL_SIGNED_APKS_MANIFEST"));
  for (std::string line; std::getline(in, line);) {
    std::istringstream fields(line);
    Expected e;
    std::string signers;
    if (!(fields >> e.file >> e.status >> e.scheme >> signers))
      continue;
    if (signers != "-") {
      std::istringstream list(signers);
      for (std::string one; std::getline(list, one, ',');)
        e.signers.push_back(one);
    }
    out.push_back(std::move(e));
  }
  return out;
}

SigResult verify_file(const std::string& file) {
  ZipError err;
  auto apk = Apk::open(env("ACL_SIGNED_APKS_DIR") + "/" + file, err);
  REQUIRE(apk.has_value());
  return verify_signature(*apk);
}

SigResult with_status(SigStatus status) {
  SigResult r;
  r.status = status;
  if (status == SigStatus::Verified) {
    r.scheme = 3;
    r.signers.push_back({{}, std::string(64, 'a'), 0x0201});
  }
  return r;
}

}  // namespace

#if ACL_HAVE_LIBCRYPTO

TEST_CASE("every signed, mis-signed and tampered APK gets its verdict",
          "[pkg][verify]") {
  const std::vector<Expected> manifest = load_manifest();
  if (manifest.empty()) {
    SKIP("signed-APK fixtures not built (python3 or openssl missing)");
  }
  // The generator emits this many; a short manifest means a fixture was lost.
  REQUIRE(manifest.size() == 41);
  for (const Expected& e : manifest) {
    INFO(e.file);
    const SigResult r = verify_file(e.file);
    INFO(r.message);
    CHECK(std::string(sig_status_name(r.status)) == e.status);
    CHECK(std::string(sig_scheme_name(r.scheme)) == e.scheme);
    REQUIRE(r.signers.size() == e.signers.size());
    for (std::size_t i = 0; i < e.signers.size(); ++i) {
      CHECK(r.signers[i].cert_sha256 == e.signers[i]);
      CHECK_FALSE(r.signers[i].cert_der.empty());
    }
    // Nothing but Verified may carry a signer a caller could pin against.
    if (!r.verified()) {
      CHECK(r.signers.empty());
      CHECK(r.scheme == 0);
      CHECK_FALSE(r.message.empty());
    }
  }
}

TEST_CASE("a v3 signer is selected by the platform SDK", "[pkg][verify]") {
  if (load_manifest().empty()) {
    SKIP("signed-APK fixtures not built (python3 or openssl missing)");
  }
  ZipError err;
  const std::string dir = env("ACL_SIGNED_APKS_DIR");
  auto rotated = Apk::open(dir + "/ok-v31-rotated.apk", err);
  REQUIRE(rotated.has_value());
  // v3.1 takes over at SDK 33; below it the original v3 signer still applies.
  const SigResult at33 = verify_signature(*rotated, 33);
  const SigResult at32 = verify_signature(*rotated, 32);
  REQUIRE(at33.verified());
  REQUIRE(at32.verified());
  CHECK(at33.scheme == 31);
  CHECK(at32.scheme == 3);
  CHECK(at33.signers[0].cert_sha256 != at32.signers[0].cert_sha256);

  auto v3 = Apk::open(dir + "/ok-v3.apk", err);
  REQUIRE(v3.has_value());
  CHECK(verify_signature(*v3, 24).verified());
  CHECK(verify_signature(*v3, 23).status == SigStatus::Invalid);
}

#else  // !ACL_HAVE_LIBCRYPTO

TEST_CASE("without libcrypto nothing is reported as verified",
          "[pkg][verify]") {
  const std::string path = env("ACL_FIXTURE_APK");
  if (path.empty()) {
    SKIP("fixture.apk not built");
  }
  ZipError err;
  auto apk = Apk::open(path, err);
  REQUIRE(apk.has_value());
  const SigResult r = verify_signature(*apk);
  CHECK(r.status == SigStatus::Unavailable);
  CHECK(r.signers.empty());
  CHECK_FALSE(admits({VerifyMode::Require, {}}, r));
}

#endif  // ACL_HAVE_LIBCRYPTO

TEST_CASE("certificate digests are validated and compared loosely",
          "[pkg][verify]") {
  const std::string lower(64, 'a');
  std::string colons;
  for (int i = 0; i < 32; ++i)
    colons += i ? ":AA" : "AA";

  CHECK(valid_cert_digest(lower));
  CHECK(valid_cert_digest(colons));
  CHECK_FALSE(valid_cert_digest(""));
  CHECK_FALSE(valid_cert_digest("00"));
  CHECK_FALSE(valid_cert_digest(std::string(63, 'a')));
  CHECK_FALSE(valid_cert_digest(std::string(64, 'g')));

  const SigResult ok = with_status(SigStatus::Verified);
  CHECK(signer_matches(ok, lower));
  CHECK(signer_matches(ok, colons));
  CHECK_FALSE(signer_matches(ok, std::string(64, 'b')));
  // A digest left on a result that did not verify must never match.
  SigResult stale = ok;
  stale.status = SigStatus::Invalid;
  CHECK_FALSE(signer_matches(stale, lower));
}

TEST_CASE("the policy admits exactly what each mode promises",
          "[pkg][verify]") {
  using S = SigStatus;
  const S all[] = {S::Verified, S::Unsigned, S::Invalid, S::Unsupported,
                   S::Unavailable};
  for (const S s : all) {
    INFO(sig_status_name(s));
    const SigResult r = with_status(s);
    CHECK(admits({VerifyMode::Off, {}}, r));
    CHECK(admits({VerifyMode::Warn, {}}, r) == (s != S::Invalid));
    CHECK(admits({VerifyMode::Require, {}}, r) == (s == S::Verified));
    // A pin overrides every mode, including off.
    for (const VerifyMode m :
         {VerifyMode::Off, VerifyMode::Warn, VerifyMode::Require}) {
      CHECK(admits({m, {std::string(64, 'a')}}, r) == (s == S::Verified));
      CHECK_FALSE(admits({m, {std::string(64, 'b')}}, r));
    }
  }
  // Any one of several pins is enough.
  CHECK(admits({VerifyMode::Warn, {std::string(64, 'b'), std::string(64, 'a')}},
               with_status(S::Verified)));

  VerifyMode m = VerifyMode::Off;
  CHECK(parse_verify_mode("require", m));
  CHECK(m == VerifyMode::Require);
  CHECK(parse_verify_mode("warn", m));
  CHECK(parse_verify_mode("off", m));
  CHECK_FALSE(parse_verify_mode("", m));
  CHECK_FALSE(parse_verify_mode("Require", m));
}
