# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §5 (Tier A unit). No GPL-3.0 sources.
#
# Tier A unit tests (Catch2), one growing test binary. Catch2 is pulled via
# subprojects/catch2.wrap and falls back gracefully if unavailable so the
# skeleton still configures.
catch2_dep = dependency(
  'catch2-with-main',
  fallback : ['catch2', 'catch2_with_main_dep'],
  required : false,
)

unit_test_sources = files(
  'smoke_test.cpp',
  'log_test.cpp',
  'ipc_test.cpp',
  'runtime_test.cpp',
  'pkg_test.cpp',
  'apk_stage_test.cpp',
  'apk_verify_test.cpp',
  'manifest_test.cpp',
  'dex_test.cpp',
  'gfx_test.cpp',
)

# Automotive-only: the media bridge (hosts/media) is compiled out under the phone
# profile, so its unit tests only build when the profile is ON.
if profile_automotive
  unit_test_sources += files('media_bridge_test.cpp', 'cal_host_test.cpp')
endif

# Build-time fixtures for the pkg/ reader tests (no binaries committed to git).
# If python3 is unavailable the pkg tests SKIP at runtime (env var unset).
python3 = find_program('python3', required : false)
test_env = environment()
fixture_deps = []
if python3.found()
  # fixture.apk embeds a UTF-8 AXML AndroidManifest.xml (make_fixture_apk.py
  # imports make_axml.py, so declare it as a dependency for rebuilds).
  fixture_apk = custom_target(
    'fixture-apk',
    input : files('../fixtures/make_fixture_apk.py'),
    output : 'fixture.apk',
    command : [python3, '@INPUT@', '@OUTPUT@'],
    depend_files : files('../fixtures/make_axml.py'),
  )
  # Standalone UTF-16 AXML to exercise the UTF-16 string-pool decode path.
  axml_utf16 = custom_target(
    'axml-utf16',
    input : files('../fixtures/make_axml.py'),
    output : 'manifest_utf16.axml',
    command : [python3, '@INPUT@', '--utf16', '@OUTPUT@'],
  )
  # Minimal valid classes.dex for the dex/ parser and DEX->JVM converter tests.
  fixture_dex = custom_target(
    'fixture-dex',
    input : files('../fixtures/make_dex.py'),
    output : 'classes.dex',
    command : [python3, '@INPUT@', '@OUTPUT@'],
  )
  test_env.set('ACL_FIXTURE_APK', fixture_apk.full_path())
  test_env.set('ACL_AXML_UTF16', axml_utf16.full_path())
  test_env.set('ACL_FIXTURE_DEX', fixture_dex.full_path())
  fixture_deps = [fixture_apk, axml_utf16, fixture_dex]

  # Signed, mis-signed and tampered APKs for pkg/apk_verify (ADR-0009 C1). Keys are
  # generated at build time by the `openssl` command, so no key or binary is committed.
  # The APKs land in a directory beside the manifest, which is the declared output.
  openssl_prog = find_program('openssl', required : false)
  have_signed_apks = openssl_prog.found()
  if have_signed_apks
    signed_apks_dir = meson.current_build_dir() / 'signed-apks'
    signed_apks = custom_target(
      'signed-apks',
      input : files('../fixtures/make_signed_apks.py'),
      output : 'signed-apks.manifest',
      command : [python3, '@INPUT@', signed_apks_dir, '@OUTPUT@'],
    )
    test_env.set('ACL_SIGNED_APKS_DIR', signed_apks_dir)
    test_env.set('ACL_SIGNED_APKS_MANIFEST', signed_apks.full_path())
    fixture_deps += signed_apks
  else
    warning('openssl not found; pkg/ signature tests will SKIP.')
  endif
else
  have_signed_apks = false
  warning('python3 not found; pkg/ fixture tests will SKIP.')
endif

if catch2_dep.found()
  unit_tests = executable(
    'acl_unit_tests',
    unit_test_sources,
    include_directories : [config_inc, core_inc],
    link_with : module_libs,
    dependencies : [catch2_dep, fmt_dep, jni_dep],
  )
  # `depends` ensures the fixture is built before the test runs.
  test('unit', unit_tests, env : test_env, depends : fixture_deps)

  # Isolated JVM integration test (one HotSpot VM per process). Built only when a
  # JDK (have_jni) and javac are available; otherwise it SKIPs at runtime.
  # Use the javac from the SAME java_home as the detected libjvm — a javac from a
  # newer JDK on PATH would emit class files the embedded (older) VM cannot load.
  javac = have_jni ? find_program(java_home / 'bin' / 'javac', required : false) \
                   : disabler()
  if have_jni and javac.found()
    jvm_classes = custom_target(
      'jvm-fixture-classes',
      input : files('../fixtures/jvm/Main.java', '../fixtures/jvm/Boom.java',
                    '../fixtures/jvm/Runner.java'),
      output : 'jvm-classes',
      command : [javac, '-d', '@OUTPUT@', '@INPUT@'],
    )
    jvm_test_env = environment()
    jvm_test_env.set('ACL_JVM_CLASSES', jvm_classes.full_path())
    jvm_depends = [jvm_classes]
    # Also feed the DEX fixture so the test exercises the DEX->JVM path.
    if python3.found()
      jvm_test_env.set('ACL_FIXTURE_DEX', fixture_dex.full_path())
      jvm_depends += fixture_dex
    endif
    jvm_test = executable(
      'acl_jvm_test',
      files('jvm_test.cpp'),
      include_directories : [config_inc, core_inc],
      link_with : module_libs,
      dependencies : [catch2_dep, fmt_dep, jni_dep],
    )
    test('jvm-invoke', jvm_test, env : jvm_test_env, depends : jvm_depends)
  else
    message('JDK/javac not both available; skipping jvm-invoke integration test.')
  endif
else
  warning('Catch2 not available; skipping Tier A unit tests. ' +
          'Provide subprojects/catch2 (meson wrap install catch2) to enable.')
endif
