# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §2, §4 Phase 1. No GPL-3.0 sources.
#
# pkg/ — APK (zip) reader + manifest/resources. Phase 1 uses a zlib-backed zip
# reader (apk.cpp) as a placeholder for AOSP libziparchive; resources.arsc and
# binary-XML manifest parsing move to libandroidfw when it is vendored.
module_libs += static_library(
  'acl_pkg',
  files('pkg.cpp', 'apk.cpp', 'apk_stage.cpp', 'apk_verify.cpp', 'manifest.cpp'),
  include_directories : [config_inc, core_inc],
  dependencies : [zlib_dep, crypto_dep],
)
