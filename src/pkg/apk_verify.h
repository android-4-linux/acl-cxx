// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l implementation of the APK Signature Scheme v2, v3
// and v3.1 verification rules, as published by AOSP (source.android.com
// "APK Signature Scheme v2/v3" and the Apache-2.0 platform verifier under
// frameworks/base/core/java/android/util/apk at android-13.0.0_r84). The format
// and the acceptance rules come from there; no code is copied. ADR-0009 C1,
// plan.md R-10. No GPL-3.0 sources.
#ifndef ACL_PKG_APK_VERIFY_H
#define ACL_PKG_APK_VERIFY_H

// One-shot verification of an APK's whole-file signature, run once when the
// APK is opened and before any byte of it is executed. It answers two
// questions: are the bytes this reader will serve the bytes the signer signed,
// and which certificate signed them. It does not answer whether that signer
// should be trusted; the caller decides that by pinning certificate digests.
//
// Not covered, by decision: v1 (JAR) signatures, the v4 / fs-verity digest,
// and validation of a v3 proof-of-rotation lineage. An APK carrying only those
// reports Unsigned or Unsupported, never Verified.
//
// Two properties a caller should know, both inherited from the scheme:
//  - Bytes inside the signing block that no signer covers (padding, unknown
//    ID-value pairs, the block of a scheme that was not selected) can change
//    without changing the verdict. Nothing the zip reader serves lives there.
//  - A key-rotated APK carries two legitimate signers. The SDK range that
//    selects between them is read before any signature is checked, so editing
//    it can make this report the original signer instead of the rotated one.
//    The content is the same either way; a pin on the other signer refuses.
//
// What this does not yet give: the runtime opens the APK again by path, so the
// verified mapping is not the only read. Whoever can replace the file between
// the two reads defeats the check.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "pkg/apk.h"

namespace acl::pkg {

// The API level the vendored framework is built at (android-13.0.0_r84). A v3
// signer is selected by the SDK range it declares.
inline constexpr int kPlatformSdk = 33;

enum class SigStatus {
  Verified,     // signature valid and every signed digest matches the file
  Unsigned,     // no v2/v3 signature block in the file
  Invalid,      // a signature block is present and does not verify
  Unsupported,  // signed only with algorithms this verifier does not implement
  Unavailable,  // built without libcrypto; nothing was checked
};

const char* sig_status_name(SigStatus status) noexcept;

struct ApkSigner {
  std::vector<std::uint8_t> cert_der;  // the signing certificate, as stored
  std::string cert_sha256;             // lowercase hex of SHA-256(cert_der)
  std::uint32_t algorithm = 0;         // signature algorithm ID that verified
};

struct SigResult {
  SigStatus status = SigStatus::Unsigned;
  int scheme = 0;                     // 2, 3, or 31 for v3.1; 0 unless Verified
  std::vector<ApkSigner> signers;     // empty unless Verified
  bool has_rotation_lineage = false;  // present in the block; not validated
  std::string message;                // why, for every status but Verified
  bool verified() const noexcept { return status == SigStatus::Verified; }
};

// "v2", "v3", "v3.1", or "none".
const char* sig_scheme_name(int scheme) noexcept;

// Verify `apk` against its own signature block. Never throws; malformed input
// yields Invalid. Reads the mapping the Apk already holds and the zip layout
// the Apk already indexed.
SigResult verify_signature(const Apk& apk, int platform_sdk = kPlatformSdk);

// True if `sha256_hex` is a well-formed certificate digest: 64 hex digits,
// upper or lower case, with or without ':' separators.
bool valid_cert_digest(std::string_view sha256_hex);

// True if any verified signer's certificate digest equals `sha256_hex`.
bool signer_matches(const SigResult& result, std::string_view sha256_hex);

// What the caller does with a result.
//   Warn:    refuse Invalid; run anything else. A signature that is present
//            and broken has no innocent cause, and unsigned APKs are what
//            development builds are.
//   Require: run only Verified.
//   Off:     run everything; the caller should not verify at all.
// A non-empty `signers` list pins the certificate: only a Verified APK signed
// by one of them runs, whatever the mode.
enum class VerifyMode { Off, Warn, Require };

struct VerifyPolicy {
  VerifyMode mode = VerifyMode::Warn;
  std::vector<std::string> signers;  // pinned certificate SHA-256 digests
};

// "off" | "warn" | "require". Returns false on anything else.
bool parse_verify_mode(std::string_view name, VerifyMode& out) noexcept;

// True if an APK with `result` may run under `policy`.
bool admits(const VerifyPolicy& policy, const SigResult& result);

}  // namespace acl::pkg

#endif  // ACL_PKG_APK_VERIFY_H
