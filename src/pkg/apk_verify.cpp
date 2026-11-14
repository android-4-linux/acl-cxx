// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation; see apk_verify.h for the published
// AOSP sources the format and acceptance rules are taken from. OpenSSL
// (Apache-2.0) supplies the primitives. No GPL-3.0 sources.

#include "pkg/apk_verify.h"

#include "acl_config.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>

#if ACL_HAVE_LIBCRYPTO
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif

namespace acl::pkg {

const char* sig_status_name(SigStatus status) noexcept {
  switch (status) {
    case SigStatus::Verified:
      return "verified";
    case SigStatus::Unsigned:
      return "unsigned";
    case SigStatus::Invalid:
      return "invalid";
    case SigStatus::Unsupported:
      return "unsupported";
    case SigStatus::Unavailable:
      return "unavailable";
  }
  return "?";
}

const char* sig_scheme_name(int scheme) noexcept {
  switch (scheme) {
    case 2:
      return "v2";
    case 3:
      return "v3";
    case 31:
      return "v3.1";
    default:
      return "none";
  }
}

std::span<const std::uint8_t> signing_block(const Apk& apk) noexcept {
  // Framing only: a size, then pairs, then the same size and a 16-byte magic,
  // ending exactly where the central directory starts.
  static constexpr char kMagic[] = "APK Sig Block 42";
  const std::span<const std::uint8_t> file = apk.bytes();
  const std::uint64_t cd = apk.layout().cd_offset;
  if (cd < 32 || cd > file.size() ||
      std::memcmp(file.data() + cd - 16, kMagic, 16) != 0) {
    return {};
  }
  const auto u64_at = [&](std::uint64_t off) {
    std::uint64_t v = 0;
    for (int i = 7; i >= 0; --i)
      v = v << 8 | file[off + static_cast<std::uint64_t>(i)];
    return v;
  };
  const std::uint64_t size = u64_at(cd - 24);
  if (size < 24 || size > cd - 8)
    return {};
  const std::uint64_t start = cd - size - 8;
  if (u64_at(start) != size)
    return {};
  return file.subspan(start, size + 8);
}

namespace {

// Lowercase hex without separators, or empty if `v` is not a SHA-256 digest.
std::string normalize_digest(std::string_view v) {
  std::string out;
  out.reserve(64);
  for (const char c : v) {
    if (c == ':')
      continue;
    if (c >= '0' && c <= '9') {
      out.push_back(c);
    } else if (c >= 'a' && c <= 'f') {
      out.push_back(c);
    } else if (c >= 'A' && c <= 'F') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      return {};
    }
  }
  return out.size() == 64 ? out : std::string{};
}

}  // namespace

bool valid_cert_digest(std::string_view sha256_hex) {
  return !normalize_digest(sha256_hex).empty();
}

bool signer_matches(const SigResult& result, std::string_view sha256_hex) {
  const std::string want = normalize_digest(sha256_hex);
  if (want.empty() || !result.verified())
    return false;
  return std::ranges::any_of(result.signers, [&](const ApkSigner& s) {
    return s.cert_sha256 == want;
  });
}

bool parse_verify_mode(std::string_view name, VerifyMode& out) noexcept {
  if (name == "off") {
    out = VerifyMode::Off;
  } else if (name == "warn") {
    out = VerifyMode::Warn;
  } else if (name == "require") {
    out = VerifyMode::Require;
  } else {
    return false;
  }
  return true;
}

bool admits(const VerifyPolicy& policy, const SigResult& result) {
  if (!policy.signers.empty()) {
    return std::ranges::any_of(policy.signers, [&](const std::string& pin) {
      return signer_matches(result, pin);
    });
  }
  switch (policy.mode) {
    case VerifyMode::Off:
      return true;
    case VerifyMode::Warn:
      return result.status != SigStatus::Invalid;
    case VerifyMode::Require:
      return result.verified();
  }
  return false;
}

#if !ACL_HAVE_LIBCRYPTO

SigResult verify_signature(const Apk& /*apk*/, int /*platform_sdk*/) {
  SigResult r;
  r.status = SigStatus::Unavailable;
  r.message = "built without libcrypto; no signature was checked";
  return r;
}

#else  // ACL_HAVE_LIBCRYPTO

namespace {

using Bytes = std::span<const std::uint8_t>;

constexpr std::uint32_t kBlockV2 = 0x7109871a;
constexpr std::uint32_t kBlockV3 = 0xf05368c0;
constexpr std::uint32_t kBlockV31 = 0x1b93ad61;
constexpr char kBlockMagic[] = "APK Sig Block 42";  // 16 bytes, no NUL on disk
constexpr std::size_t kMagicSize = 16;
constexpr std::size_t kBlockFooterSize = 8 + kMagicSize;  // size + magic
constexpr std::size_t kEocdMinSize = 22;
constexpr std::size_t kEocdCdOffsetField = 16;
constexpr std::size_t kMaxV2Signers = 10;
constexpr std::size_t kChunkSize = 1024 * 1024;
// Keys and certificates are a few kilobytes. The bound keeps a hostile length
// out of the `long` the DER parsers take.
constexpr std::size_t kMaxDerSize = 1024 * 1024;

// Signed-data additional attribute IDs.
constexpr std::uint32_t kAttrV2StrippingProtection = 0xbeeff00d;
constexpr std::uint32_t kAttrProofOfRotation = 0x3ba06f8c;
constexpr std::uint32_t kAttrRotationMinSdk = 0x559f8b02;
constexpr std::uint32_t kAttrRotationOnDevRelease = 0xc2a6b3ba;

// Content digest kinds, ordered by strength for selection.
enum class Digest { None = 0, Sha256 = 1, Sha512 = 2 };

enum class KeyKind { Rsa, Ec, Dsa };

struct Algorithm {
  std::uint32_t id;
  Digest digest;
  KeyKind key;
  bool pss;
};

// The fs-verity IDs (0x0421, 0x0423, 0x0425) are deliberately absent: their
// content digest is a Merkle root this verifier does not compute.
constexpr Algorithm kAlgorithms[] = {
    {0x0101, Digest::Sha256, KeyKind::Rsa, true},
    {0x0102, Digest::Sha512, KeyKind::Rsa, true},
    {0x0103, Digest::Sha256, KeyKind::Rsa, false},
    {0x0104, Digest::Sha512, KeyKind::Rsa, false},
    {0x0201, Digest::Sha256, KeyKind::Ec, false},
    {0x0202, Digest::Sha512, KeyKind::Ec, false},
    {0x0301, Digest::Sha256, KeyKind::Dsa, false},
};

const Algorithm* find_algorithm(std::uint32_t id) noexcept {
  for (const Algorithm& a : kAlgorithms) {
    if (a.id == id)
      return &a;
  }
  return nullptr;
}

const EVP_MD* md_for(Digest d) noexcept {
  return d == Digest::Sha512 ? EVP_sha512() : EVP_sha256();
}

std::size_t digest_size(Digest d) noexcept {
  return d == Digest::Sha512 ? 64 : 32;
}

// A bounds-checked little-endian cursor. Every read reports failure instead of
// running past the end; the caller turns that into Invalid.
class Reader {
 public:
  explicit Reader(Bytes b) : b_(b) {}
  bool empty() const noexcept { return b_.empty(); }
  std::size_t remaining() const noexcept { return b_.size(); }

  bool u32(std::uint32_t& out) noexcept {
    if (b_.size() < 4)
      return false;
    out = static_cast<std::uint32_t>(b_[0]) |
          static_cast<std::uint32_t>(b_[1]) << 8 |
          static_cast<std::uint32_t>(b_[2]) << 16 |
          static_cast<std::uint32_t>(b_[3]) << 24;
    b_ = b_.subspan(4);
    return true;
  }
  bool u64(std::uint64_t& out) noexcept {
    std::uint32_t lo = 0;
    std::uint32_t hi = 0;
    if (b_.size() < 8 || !u32(lo) || !u32(hi))
      return false;
    out = static_cast<std::uint64_t>(hi) << 32 | lo;
    return true;
  }
  bool take(std::size_t n, Bytes& out) noexcept {
    if (b_.size() < n)
      return false;
    out = b_.first(n);
    b_ = b_.subspan(n);
    return true;
  }
  // A uint32 length followed by that many bytes.
  bool slice(Bytes& out) noexcept {
    std::uint32_t n = 0;
    return u32(n) && take(n, out);
  }

 private:
  Bytes b_;
};

struct EvpPkeyFree {
  void operator()(EVP_PKEY* p) const noexcept { EVP_PKEY_free(p); }
};
struct X509Free {
  void operator()(X509* p) const noexcept { X509_free(p); }
};
struct MdCtxFree {
  void operator()(EVP_MD_CTX* p) const noexcept { EVP_MD_CTX_free(p); }
};
using PkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyFree>;
using X509Ptr = std::unique_ptr<X509, X509Free>;
using MdCtxPtr = std::unique_ptr<EVP_MD_CTX, MdCtxFree>;

std::string hex(Bytes b) {
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string s;
  s.reserve(b.size() * 2);
  for (const std::uint8_t v : b) {
    s.push_back(kDigits[v >> 4]);
    s.push_back(kDigits[v & 0xf]);
  }
  return s;
}

// Outcome of verifying one signer.
struct Step {
  bool ok = false;
  bool skip = false;         // signer is for another platform; not a failure
  bool unsupported = false;  // signer uses only algorithms not implemented
  std::string why;
  static Step pass() { return {true, false, false, {}}; }
  static Step not_for_platform() { return {false, true, false, {}}; }
  static Step fail(std::string why) {
    return {false, false, false, std::move(why)};
  }
  static Step no_algorithm() {
    return {false, false, true, "no supported signature algorithm"};
  }
};

bool key_matches_kind(const EVP_PKEY* key, KeyKind kind) noexcept {
  const int id = EVP_PKEY_get_base_id(key);
  switch (kind) {
    case KeyKind::Rsa:
      return id == EVP_PKEY_RSA;
    case KeyKind::Ec:
      return id == EVP_PKEY_EC;
    case KeyKind::Dsa:
      return id == EVP_PKEY_DSA;
  }
  return false;
}

bool verify_raw_signature(EVP_PKEY* key,
                          const Algorithm& alg,
                          Bytes data,
                          Bytes sig) noexcept {
  const MdCtxPtr ctx(EVP_MD_CTX_new());
  if (!ctx)
    return false;
  EVP_PKEY_CTX* pctx = nullptr;  // owned by ctx
  const EVP_MD* md = md_for(alg.digest);
  if (EVP_DigestVerifyInit(ctx.get(), &pctx, md, nullptr, key) != 1)
    return false;
  if (alg.pss) {
    // Salt length equals the digest length, MGF1 over the same digest.
    if (EVP_PKEY_CTX_set_rsa_padding(pctx, RSA_PKCS1_PSS_PADDING) != 1 ||
        EVP_PKEY_CTX_set_rsa_pss_saltlen(
            pctx, static_cast<int>(digest_size(alg.digest))) != 1 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(pctx, md) != 1) {
      return false;
    }
  }
  return EVP_DigestVerify(ctx.get(), sig.data(), sig.size(), data.data(),
                          data.size()) == 1;
}

// State carried across the blocks of one APK.
struct Context {
  int platform_sdk = kPlatformSdk;
  // The lowest minSdk among v3.1 signers that did not apply to this platform.
  // A v3 block may require it (stripping protection for v3.1).
  std::optional<std::int32_t> rotation_min_sdk;
  // Digest kind -> the signed content digest every signer must agree on.
  std::map<Digest, std::vector<std::uint8_t>> content_digests;
  bool has_rotation_lineage = false;
};

// Verify one signer record. `scheme` is 2, 3 or 31.
Step verify_signer(Bytes signer, int scheme, Context& ctx, ApkSigner& out) {
  const bool v3 = scheme != 2;
  Reader r(signer);
  Bytes signed_data;
  if (!r.slice(signed_data))
    return Step::fail("malformed signer: signed data");

  std::uint32_t min_sdk_u = 0;
  std::uint32_t max_sdk_u = 0;
  if (v3) {
    if (!r.u32(min_sdk_u) || !r.u32(max_sdk_u))
      return Step::fail("malformed signer: SDK range");
    const auto min_sdk = static_cast<std::int32_t>(min_sdk_u);
    const auto max_sdk = static_cast<std::int32_t>(max_sdk_u);
    if (ctx.platform_sdk < min_sdk || ctx.platform_sdk > max_sdk) {
      if (scheme == 31 &&
          (!ctx.rotation_min_sdk || *ctx.rotation_min_sdk > min_sdk)) {
        ctx.rotation_min_sdk = min_sdk;
      }
      return Step::not_for_platform();
    }
  }

  Bytes signatures;
  Bytes public_key;
  if (!r.slice(signatures) || !r.slice(public_key))
    return Step::fail("malformed signer: signatures or public key");

  // Pick the strongest signature this verifier implements. The list of
  // algorithm IDs must match the digest list below, in order.
  std::vector<std::uint32_t> sig_algorithms;
  const Algorithm* best = nullptr;
  Bytes best_sig;
  for (Reader sigs(signatures); !sigs.empty();) {
    Bytes record;
    if (!sigs.slice(record) || record.size() < 8)
      return Step::fail("malformed signature record");
    Reader rec(record);
    std::uint32_t id = 0;
    rec.u32(id);
    sig_algorithms.push_back(id);
    const Algorithm* alg = find_algorithm(id);
    if (alg == nullptr)
      continue;
    if (best == nullptr || alg->digest > best->digest) {
      Bytes sig;
      if (!rec.slice(sig))
        return Step::fail("malformed signature record");
      best = alg;
      best_sig = sig;
    }
  }
  if (sig_algorithms.empty())
    return Step::fail("no signatures in signer");
  if (best == nullptr)
    return Step::no_algorithm();

  if (public_key.size() > kMaxDerSize)
    return Step::fail("public key too large");
  const std::uint8_t* kp = public_key.data();
  const PkeyPtr key(
      d2i_PUBKEY(nullptr, &kp, static_cast<long>(public_key.size())));
  if (!key || kp != public_key.data() + public_key.size())
    return Step::fail("malformed public key");
  if (!key_matches_kind(key.get(), best->key))
    return Step::fail("public key type does not match signature algorithm");
  if (!verify_raw_signature(key.get(), *best, signed_data, best_sig))
    return Step::fail("signature over signed data did not verify");

  // The signature holds. Everything below is read from signed data.
  Reader sd(signed_data);
  Bytes digests;
  Bytes certificates;
  if (!sd.slice(digests) || !sd.slice(certificates))
    return Step::fail("malformed signed data");

  std::vector<std::uint32_t> digest_algorithms;
  Bytes content_digest;
  bool have_digest = false;
  for (Reader ds(digests); !ds.empty();) {
    Bytes record;
    if (!ds.slice(record) || record.size() < 8)
      return Step::fail("malformed digest record");
    Reader rec(record);
    std::uint32_t id = 0;
    rec.u32(id);
    digest_algorithms.push_back(id);
    if (id == best->id) {
      if (!rec.slice(content_digest))
        return Step::fail("malformed digest record");
      have_digest = true;
    }
  }
  if (sig_algorithms != digest_algorithms)
    return Step::fail("signature and digest algorithm lists differ");
  if (!have_digest || content_digest.size() != digest_size(best->digest))
    return Step::fail("signed content digest has the wrong size");

  auto [it, inserted] = ctx.content_digests.try_emplace(
      best->digest, content_digest.begin(), content_digest.end());
  if (!inserted && (it->second.size() != content_digest.size() ||
                    CRYPTO_memcmp(it->second.data(), content_digest.data(),
                                  content_digest.size()) != 0)) {
    return Step::fail("signers disagree on the content digest");
  }

  // Every certificate must parse; the first one is the signer's.
  Bytes first_cert;
  X509Ptr first_x509;
  for (Reader cs(certificates); !cs.empty();) {
    Bytes der;
    if (!cs.slice(der) || der.size() > kMaxDerSize)
      return Step::fail("malformed certificate list");
    const std::uint8_t* cp = der.data();
    X509Ptr x(d2i_X509(nullptr, &cp, static_cast<long>(der.size())));
    if (!x || cp != der.data() + der.size())
      return Step::fail("malformed certificate");
    if (!first_x509) {
      first_cert = der;
      first_x509 = std::move(x);
    }
  }
  if (!first_x509)
    return Step::fail("no certificates in signed data");
  // get0: borrowed from the certificate.
  EVP_PKEY* cert_key = X509_get0_pubkey(first_x509.get());
  if (cert_key == nullptr || EVP_PKEY_eq(cert_key, key.get()) != 1)
    return Step::fail("public key does not match the signing certificate");

  if (v3) {
    std::uint32_t signed_min = 0;
    std::uint32_t signed_max = 0;
    if (!sd.u32(signed_min) || !sd.u32(signed_max))
      return Step::fail("malformed signed data: SDK range");
    if (signed_min != min_sdk_u || signed_max != max_sdk_u)
      return Step::fail("signed and unsigned SDK ranges differ");
  }

  Bytes attributes;
  if (!sd.slice(attributes))
    return Step::fail("malformed signed data: attributes");
  for (Reader as(attributes); !as.empty();) {
    Bytes attr;
    std::uint32_t id = 0;
    if (!as.slice(attr))
      return Step::fail("malformed attribute");
    Reader a(attr);
    if (!a.u32(id))
      return Step::fail("malformed attribute");
    if (!v3 && id == kAttrV2StrippingProtection) {
      // The v2 signer declares a v3 signature exists. This path only runs
      // when none was found, so it was removed.
      std::uint32_t declared = 0;
      if (!a.u32(declared))
        return Step::fail("malformed stripping-protection attribute");
      if (declared == 3)
        return Step::fail("v2 signer declares a v3 signature; none is present");
    } else if (v3 && id == kAttrProofOfRotation) {
      ctx.has_rotation_lineage = true;
    } else if (v3 && id == kAttrRotationMinSdk) {
      std::uint32_t declared = 0;
      if (!a.u32(declared))
        return Step::fail("malformed rotation-min-sdk attribute");
      if (!ctx.rotation_min_sdk ||
          static_cast<std::uint32_t>(*ctx.rotation_min_sdk) != declared) {
        return Step::fail(
            "v3 signer declares a v3.1 block that is absent or differs");
      }
    } else if (scheme == 31 && id == kAttrRotationOnDevRelease) {
      // A signer for a platform still in development. This is a release
      // platform, so at its own minSdk the signer does not apply.
      if (ctx.platform_sdk == static_cast<std::int32_t>(min_sdk_u)) {
        ctx.rotation_min_sdk = static_cast<std::int32_t>(min_sdk_u);
        return Step::not_for_platform();
      }
    }
  }

  std::uint8_t cert_hash[32];
  unsigned int cert_hash_len = 0;
  if (EVP_Digest(first_cert.data(), first_cert.size(), cert_hash,
                 &cert_hash_len, EVP_sha256(), nullptr) != 1) {
    return Step::fail("certificate digest failed");
  }
  out.cert_der.assign(first_cert.begin(), first_cert.end());
  out.cert_sha256 = hex({cert_hash, cert_hash_len});
  out.algorithm = best->id;
  return Step::pass();
}

// The whole-file digest: every section is cut into 1 MiB chunks, each chunk is
// digested as 0xa5 || len || bytes, and the result is the digest of
// 0x5a || count || chunk digests. The signing block itself is not a section.
bool content_digest(Bytes file,
                    std::uint64_t block_offset,
                    const Apk::Layout& z,
                    Digest kind,
                    std::vector<std::uint8_t>& out) {
  // The EOCD's central-directory offset is digested as if it pointed at the
  // signing block, so the digest does not depend on the block's own size.
  std::vector<std::uint8_t> eocd(file.begin() + z.eocd_offset, file.end());
  for (int i = 0; i < 4; ++i) {
    eocd[kEocdCdOffsetField + i] =
        static_cast<std::uint8_t>(block_offset >> (8 * i));
  }
  const Bytes sections[] = {
      file.first(block_offset),
      file.subspan(z.cd_offset, z.cd_size),
      Bytes(eocd),
  };

  std::size_t chunks = 0;
  for (const Bytes s : sections)
    chunks += (s.size() + kChunkSize - 1) / kChunkSize;

  const std::size_t dlen = digest_size(kind);
  const EVP_MD* md = md_for(kind);
  std::vector<std::uint8_t> top(5 + chunks * dlen);
  top[0] = 0x5a;
  for (int i = 0; i < 4; ++i)
    top[1 + i] = static_cast<std::uint8_t>(chunks >> (8 * i));

  const MdCtxPtr ctx(EVP_MD_CTX_new());
  if (!ctx)
    return false;
  std::size_t index = 0;
  for (const Bytes s : sections) {
    for (std::size_t off = 0; off < s.size(); off += kChunkSize, ++index) {
      const std::size_t n = std::min(kChunkSize, s.size() - off);
      const std::uint8_t prefix[5] = {
          0xa5,
          static_cast<std::uint8_t>(n),
          static_cast<std::uint8_t>(n >> 8),
          static_cast<std::uint8_t>(n >> 16),
          static_cast<std::uint8_t>(n >> 24),
      };
      unsigned int got = 0;
      if (EVP_DigestInit_ex(ctx.get(), md, nullptr) != 1 ||
          EVP_DigestUpdate(ctx.get(), prefix, sizeof(prefix)) != 1 ||
          EVP_DigestUpdate(ctx.get(), s.data() + off, n) != 1 ||
          EVP_DigestFinal_ex(ctx.get(), top.data() + 5 + index * dlen, &got) !=
              1 ||
          got != dlen) {
        return false;
      }
    }
  }
  out.resize(dlen);
  unsigned int got = 0;
  return EVP_Digest(top.data(), top.size(), out.data(), &got, md, nullptr) ==
             1 &&
         got == dlen;
}

struct BlockOutcome {
  enum class Kind { Verified, NotForPlatform, Invalid, Unsupported };
  Kind kind = Kind::Invalid;
  std::string why;
  std::vector<ApkSigner> signers;
};

BlockOutcome verify_block(Bytes block, int scheme, Context& ctx) {
  using Kind = BlockOutcome::Kind;
  BlockOutcome o;
  Reader top(block);
  Bytes signers;
  if (!top.slice(signers)) {
    o.why = "malformed signer list";
    return o;
  }
  bool unsupported = false;
  for (Reader ss(signers); !ss.empty();) {
    Bytes signer;
    if (!ss.slice(signer)) {
      o.why = "malformed signer list";
      return o;
    }
    if (scheme == 2 && o.signers.size() >= kMaxV2Signers) {
      o.why = "more than 10 v2 signers";
      return o;
    }
    ApkSigner s;
    const Step step = verify_signer(signer, scheme, ctx, s);
    if (step.skip)
      continue;
    if (step.unsupported) {
      unsupported = true;
      o.why = step.why;
      continue;
    }
    if (!step.ok) {
      o.why =
          "signer " + std::to_string(o.signers.size() + 1) + ": " + step.why;
      return o;
    }
    o.signers.push_back(std::move(s));
  }
  if (unsupported) {
    // A signer no algorithm here can check. Its consent is unknown, so the
    // APK is not Verified even if another signer passed.
    o.kind = Kind::Unsupported;
    o.signers.clear();
    return o;
  }
  if (o.signers.empty()) {
    if (scheme == 31) {
      o.kind = Kind::NotForPlatform;
    } else {
      o.why =
          scheme == 3 ? "no v3 signer applies to this platform" : "no signers";
    }
    return o;
  }
  if (scheme != 2 && o.signers.size() != 1) {
    o.why = "more than one v3 signer applies to this platform";
    o.signers.clear();
    return o;
  }
  o.kind = Kind::Verified;
  return o;
}

}  // namespace

SigResult verify_signature(const Apk& apk, int platform_sdk) {
  SigResult result;
  const Bytes file = apk.bytes();
  const Apk::Layout& z = apk.layout();

  // Locate the signing block: it ends immediately before the central
  // directory, with its size and a 16-byte magic as the last 24 bytes.
  if (z.cd_offset < 8 + kBlockFooterSize || z.cd_offset > file.size() ||
      std::memcmp(file.data() + z.cd_offset - kMagicSize, kBlockMagic,
                  kMagicSize) != 0) {
    result.status = SigStatus::Unsigned;
    result.message = "no APK signing block (v1 JAR signatures are not checked)";
    return result;
  }
  const auto invalid = [&](std::string why) {
    result = {};
    result.status = SigStatus::Invalid;
    result.message = std::move(why);
    return result;
  };

  // From here a block is present, so every inconsistency is Invalid. The zip
  // shape is checked first: the three digested sections must tile the file
  // outside the block, or bytes would exist that no digest covers.
  if (z.eocd_offset + kEocdMinSize > file.size())
    return invalid("end-of-central-directory out of bounds");
  const std::size_t comment_len =
      static_cast<std::size_t>(file[z.eocd_offset + 20]) |
      static_cast<std::size_t>(file[z.eocd_offset + 21]) << 8;
  if (z.eocd_offset + kEocdMinSize + comment_len != file.size())
    return invalid("data after the end-of-central-directory record");
  if (z.cd_offset + z.cd_size != z.eocd_offset)
    return invalid("gap between central directory and its end record");

  Reader footer(file.subspan(z.cd_offset - kBlockFooterSize, 8));
  std::uint64_t size_in_footer = 0;
  footer.u64(size_in_footer);
  if (size_in_footer < kBlockFooterSize || size_in_footer > z.cd_offset - 8)
    return invalid("signing block size out of range");
  const std::uint64_t block_offset = z.cd_offset - size_in_footer - 8;
  Reader header(file.subspan(block_offset, 8));
  std::uint64_t size_in_header = 0;
  header.u64(size_in_header);
  if (size_in_header != size_in_footer)
    return invalid("signing block header and footer sizes differ");

  // ID-value pairs between the header size and the footer.
  Bytes v2;
  Bytes v3;
  Bytes v31;
  bool have_v2 = false;
  bool have_v3 = false;
  bool have_v31 = false;
  for (Reader pairs(
           file.subspan(block_offset + 8, size_in_footer - kBlockFooterSize));
       !pairs.empty();) {
    std::uint64_t len = 0;
    Bytes pair;
    std::uint32_t id = 0;
    if (!pairs.u64(len) || len < 4 || len > pairs.remaining() ||
        !pairs.take(static_cast<std::size_t>(len), pair)) {
      return invalid("malformed signing block entry");
    }
    Reader p(pair);
    p.u32(id);
    const Bytes value = pair.subspan(4);
    if (id == kBlockV2 && !have_v2) {
      v2 = value;
      have_v2 = true;
    } else if (id == kBlockV3 && !have_v3) {
      v3 = value;
      have_v3 = true;
    } else if (id == kBlockV31 && !have_v31) {
      v31 = value;
      have_v31 = true;
    }
  }
  if (!have_v2 && !have_v3 && !have_v31) {
    result.status = SigStatus::Unsigned;
    result.message = "signing block holds no v2 or v3 signature";
    return result;
  }

  // Newest applicable scheme wins, and a scheme that is present but fails
  // never falls back to an older one.
  using Kind = BlockOutcome::Kind;
  Context ctx;
  ctx.platform_sdk = platform_sdk;
  BlockOutcome outcome;
  int scheme = 0;
  if (have_v31) {
    scheme = 31;
    outcome = verify_block(v31, 31, ctx);
  }
  const bool older =
      !have_v31 || outcome.kind == Kind::NotForPlatform;  // not on failure
  if (older && (have_v3 || have_v2)) {
    scheme = have_v3 ? 3 : 2;
    ctx.content_digests.clear();
    outcome = verify_block(have_v3 ? v3 : v2, scheme, ctx);
  }
  const std::string label = sig_scheme_name(scheme);
  if (outcome.kind == Kind::Unsupported) {
    result.status = SigStatus::Unsupported;
    result.message = label + ": " + outcome.why;
    return result;
  }
  if (outcome.kind == Kind::NotForPlatform)
    return invalid(label + ": no signer applies to this platform");
  if (outcome.kind != Kind::Verified)
    return invalid(label + ": " + outcome.why);

  // The signatures hold. Now the file must match what they signed.
  if (ctx.content_digests.empty())
    return invalid(label + ": no content digest");
  for (const auto& [kind, expected] : ctx.content_digests) {
    std::vector<std::uint8_t> actual;
    if (!content_digest(file, block_offset, z, kind, actual))
      return invalid(label + ": content digest could not be computed");
    if (actual.size() != expected.size() ||
        CRYPTO_memcmp(actual.data(), expected.data(), actual.size()) != 0) {
      return invalid(label + ": file contents do not match the signed digest");
    }
  }

  result.status = SigStatus::Verified;
  result.scheme = scheme;
  result.signers = std::move(outcome.signers);
  result.has_rotation_lineage = ctx.has_rotation_lineage;
  return result;
}

#endif  // ACL_HAVE_LIBCRYPTO

}  // namespace acl::pkg
