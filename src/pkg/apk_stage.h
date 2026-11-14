// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l design. ADR-0009 C1 (the verified read must be the
// only read), plan.md R-10, docs/ndk-scope.md trust model. No GPL-3.0 sources.
#ifndef ACL_PKG_APK_STAGE_H
#define ACL_PKG_APK_STAGE_H

// Where a4l keeps what it extracts from an APK, and how an APK whose signature
// must hold is run from bytes nobody else can change.
//
// verify_signature() checks one mapping of the file. The runtime then opens
// the APK again by path, more than once, and loads native libraries extracted
// from it. If the path can be written by anyone else, what was verified and
// what runs can differ. stage_verified() closes that: it copies the APK into a
// directory only this user can write, verifies the copy, and hands back the
// copy. Everything after that reads the copy. The original is used only as the
// source of the copy and is never trusted.
//
// The boundary is the user ID. A process running as the same user can rewrite
// the cache; that is the absence of a sandbox (ADR-0009 C4), not this.

#include <optional>
#include <string>
#include <string_view>

#include "pkg/apk.h"
#include "pkg/apk_verify.h"

namespace acl::pkg {

// The directory a4l owns for per-user state: $XDG_CACHE_HOME/acl, else
// $HOME/.cache/acl, else /tmp/acl-<uid>. Created 0700 if missing. Returns ""
// and fills `error` unless it is a real directory, owned by this user, that
// neither group nor others can write: a cache someone else can write is not a
// place to load code from.
std::string private_cache_root(std::string& error);

// `package` if it is a well-formed Android package name, else "app". The name
// comes from the APK's manifest and becomes a directory name, so anything that
// could leave the directory it is joined to is rejected.
std::string safe_package_dir(std::string_view package);

// Extract lib/<abi>/*.so from `apk` into `lib_dir`, replacing what is there.
// Each file is written under a temporary name and renamed into place, so a
// concurrent run never maps a half-written library, and files the APK does not
// contain are removed, so a library left by an earlier version cannot load.
// Returns false and fills `error` on the first failure; `count` is the number
// extracted.
bool extract_native_libs(const Apk& apk,
                         std::string_view abi,
                         const std::string& lib_dir,
                         int& count,
                         std::string& error);

struct StagedApk {
  Apk apk;              // the private copy, opened; read this, not the source
  std::string dir;      // <cache_root>/verified/<key>
  std::string lib_dir;  // <dir>/lib; not created until libraries are extracted
  bool reused = false;  // an earlier run's copy was verified again and used
  // A shared lock on the directory. Held until the process exits so that a
  // later run's eviction leaves this copy alone. Deliberately never closed.
  int lock_fd = -1;
};

struct StageResult {
  std::optional<StagedApk> staged;  // set only if the copy verified
  SigResult sig;      // the copy's result if staged, else the source's
  std::string error;  // an I/O or cache failure, as opposed to a bad signature
};

// Copy `source` into `cache_root`, verify the copy and return it. A copy from
// an earlier run is verified again and reused; one that no longer verifies is
// discarded and remade. Unused copies beyond the most recent few are removed.
// Not staged when the APK does not verify: `sig` then says why.
StageResult stage_verified(const Apk& source,
                           const std::string& cache_root,
                           int platform_sdk = kPlatformSdk);

}  // namespace acl::pkg

#endif  // ACL_PKG_APK_STAGE_H
