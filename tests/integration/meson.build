# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §5 (Tier B headless integration). No GPL-3.0 sources.
#
# Tier B run-path integration: build a self-contained hello-world APK and exec the
# real `acl` binary on it, asserting the whole CLI -> pkg -> manifest -> dex ->
# DEX/JVM -> JVM -> app-output vertical runs. Needs python3 (fixture) and a JDK
# (have_jni, so the jvm backend can execute); otherwise the test is skipped.
py3 = find_program('python3', required : false)

# Gate G2.c: the frameworks/base subsetting tool. Pure static analysis over a
# synthetic AOSP-shaped source tree — needs only python3 (no JVM), so it runs even
# where the JDK is absent.
if py3.found()
  test(
    'fw-extract',
    py3,
    args : [files('fw_extract_test.py'), files('../../tools/fw_extract.py'),
            meson.current_source_dir() / '../fixtures/fwsrc'],
    timeout : 60,
  )

  # The bionic struct offsets the per-thread attach path needs (docs/ndk-scope.md). The no-AOSP
  # half runs everywhere and pins that a checkout-less build stays green; the measuring half runs
  # only where $AOSP has bionic sources, and pins the slot ABI + struct bounds — a wrong offset
  # would not fail a build, it would corrupt a hand-built TCB.
  test(
    'bionic-offsets',
    py3,
    args : [files('bionic_offsets_test.py'), meson.project_source_root()],
    timeout : 300,
  )

  # Per-thread attach-as-bionic: a glibc-created thread, handed a hand-built bionic TCB, runs real
  # bionic code as itself (own TCB + pthread_internal_t + tid). Needs a staged bionic + the offsets
  # header; SKIPs (exit 77) when absent. See docs/ndk-scope.md.
  test(
    'ndk-attach',
    py3,
    args : [files('run_ndk_attach_test.py'), ndk_probe_exe, meson.project_source_root()],
    depends : [ndk_probe_exe],
    timeout : 300,
  )

  # acl's own libandroid.so: the ALooper family, driven from a bionic .so that links it exactly as
  # a real engine does. Needs a staged bionic ($AOSP); SKIPs (exit 77) when absent.
  test(
    'ndk-libandroid',
    py3,
    args : [files('run_ndk_libandroid_test.py'), ndk_probe_exe, meson.project_source_root()],
    depends : [ndk_probe_exe],
    timeout : 300,
  )

  # APK signature policy as the CLI exposes it (ADR-0009 C1): `a4l verify` and `a4l run` under
  # each --verify mode, a pinned signer and the environment seams, asserted on exit codes. Needs
  # the signed fixtures (openssl) and a libcrypto build; --vm=null keeps a VM out of it.
  if have_signed_apks and have_libcrypto
    test(
      'verify-policy',
      py3,
      args : [files('run_verify_policy_test.py'), acl_exe, signed_apks_dir, signed_apks],
      depends : [acl_exe, signed_apks],
      timeout : 120,
    )
  endif

  # `acl tone` manual audio-check command: run silent (ACL_AUDIO=null) so it is
  # deterministic and device-independent; asserts generation + frame accounting and
  # that it never hangs. Needs only the binary.
  test(
    'tone-cli',
    py3,
    args : [files('run_tone_test.py'), acl_exe],
    depends : [acl_exe],
    timeout : 30,
  )

  # ADR-0002 vendoring orchestrator (tools/vendor_framework.py): size + vendor +
  # MANIFEST logic verified against the synthetic AOSP fixture (no real checkout).
  test(
    'vendor-framework',
    py3,
    args : [files('vendor_framework_test.py'), files('../../tools/vendor_framework.py'),
            meson.current_source_dir() / '../fixtures/fwsrc',
            meson.current_build_dir() / 'vendor-tmp'],
    timeout : 60,
  )

  # Push-button: size the AOSP frameworks/base subset for a seed against the real
  # checkout ($ACL_AOSP_ROOT, default /mnt/dev/aosp). Manual (not a gating test):
  #   ninja -C build framework-size            # default seed (android.util)
  #   tools/vendor_framework.py size --seed android.app,android.os --budget 400
  run_target(
    'framework-size',
    command : [py3, files('../../tools/vendor_framework.py'), 'size'],
  )
  # Size every package in the ADR-0002 vendoring plan in one pass (closure + shim
  # edges vs budget): ninja -C build framework-plan
  run_target(
    'framework-plan',
    command : [py3, files('../../tools/vendor_framework.py'), 'plan'],
  )
endif

# Zero-copy hardware video-decode path ([[flutter-texture-bridge]] M5): FFmpeg VAAPI decode ->
# DRM_PRIME dma-buf -> ahb_import (no copy) -> multi-plane NV12 EGLImage -> YUV->RGB sample. Generates
# a solid-color clip and asserts the probe recovers the color. SKIPs without ffmpeg/VAAPI/GL.
if py3.found() and have_gl and have_ffmpeg
  test(
    'video-decode-zerocopy',
    py3,
    args : [files('run_video_decode_test.py'), video_decode_probe],
    depends : [video_decode_probe],
    timeout : 120,
  )
  # android.media.MediaCodec native underside end-to-end ([[flutter-texture-bridge]] M5): a real
  # MediaCodec decodes H.264 into an ImageReader Surface via acl's zero-copy CodecDecoder (the path
  # video_player_android drives). Needs the vendored framework (run-apk.sh) + the AOSP toolchain +
  # ffmpeg + VAAPI; SKIPs otherwise.
  if framework_android_jar != ''
    test(
      'media-codec-decode',
      py3,
      args : [files('run_media_codec_test.py'), acl_exe, framework_android_jar],
      depends : [acl_exe],
      timeout : 300,
    )
  endif
endif

if py3.found() and have_jni
  hello_apk = custom_target(
    'hello-apk',
    input : files('../fixtures/make_hello_apk.py'),
    output : 'hello.apk',
    command : [py3, '@INPUT@', '@OUTPUT@'],
    depend_files : files('../fixtures/make_axml.py'),
  )
  test(
    'run-apk',
    py3,
    args : [files('run_apk_test.py'), acl_exe, hello_apk],
    depends : [acl_exe, hello_apk],
    timeout : 120,
  )

  # Gate G2.a: the framework boot-classpath seam. probe.apk's main() calls a class
  # (com.acl.fw.Probe) that is NOT bundled; it must come from --framework. Uses
  # the javac from the SAME java_home as the detected libjvm (class-file version).
  javac = find_program(java_home / 'bin' / 'javac', required : false)
  if javac.found()
    probe_apk = custom_target(
      'probe-apk',
      input : files('../fixtures/make_hello_apk.py'),
      output : 'probe.apk',
      command : [py3, '@INPUT@', '--probe', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    framework_classes = custom_target(
      'framework-probe-classes',
      input : files('../fixtures/framework/com/acl/fw/Probe.java'),
      output : 'framework',
      command : [javac, '-d', '@OUTPUT@', '@INPUT@'],
    )
    test(
      'run-apk-framework',
      py3,
      args : [files('run_framework_test.py'), acl_exe, probe_apk,
              framework_classes],
      depends : [acl_exe, probe_apk, framework_classes],
      timeout : 120,
    )

    # Gate G2.b: the JNI native underside. log.apk's main() calls
    # android.util.Log.i(...); the framework Log shim's native write0() is bound by
    # acl (RegisterNatives) and forwards to acl::log.
    log_apk = custom_target(
      'log-apk',
      input : files('../fixtures/make_hello_apk.py'),
      output : 'log.apk',
      command : [py3, '@INPUT@', '--log', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    # The framework boot classpath (android.* shims + vendored AOSP), built once by
    # framework/meson.build and shared here — the same artifact `acl run` uses by
    # default. (framework_classes is a real target under have_jni, which this block
    # requires.)
    framework_shim = framework_boot_classes
    test(
      'run-apk-log',
      py3,
      args : [files('run_log_test.py'), acl_exe, log_apk, framework_shim],
      depends : [acl_exe, log_apk, framework_shim],
      timeout : 120,
    )

    # Gate G2.d: the Android Activity entry point. activity.apk's launcher is
    # MainActivity extends android.app.Activity; `acl run` instantiates it and
    # drives onCreate/onStart/onResume against the shimmed framework — no main().
    activity_apk = custom_target(
      'activity-apk',
      input : files('../fixtures/make_activity_apk.py'),
      output : 'activity.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-activity',
      py3,
      args : [files('run_activity_test.py'), acl_exe, activity_apk, framework_shim],
      depends : [acl_exe, activity_apk, framework_shim],
      timeout : 120,
    )

    # The same Activity, but on REAL host ART (--vm=art): the harness d8s the vendored
    # framework + a MainActivity fixture into an ART-loadable (zipaligned) APK and
    # asserts the lifecycle runs on host libart. Needs a host ART build (ACL_ART_HOME)
    # + the AOSP host toolchain (d8/zipalign/jdk) — it SKIPs (exit 77) when absent, so
    # CI stays green without ART. Reuses activity.apk's binary manifest + android.jar.
    if framework_android_jar != ''
      test(
        'run-apk-activity-art',
        py3,
        args : [files('run_activity_art_test.py'), acl_exe,
                meson.project_source_root(), framework_android_jar, activity_apk],
        depends : [acl_exe, activity_apk],
        timeout : 300,
      )
    endif

    # ADR-5 / R-6: an app's BIONIC-ABI lib/<abi>/*.so, loaded in-process by acl's bionic
    # loader on the real ART path — System.loadLibrary from an Activity, routed through the
    # gated Runtime.nativeLoad hook, JNI_OnLoad + a full down/up-call round trip (=4250).
    # Needs a host ART build (ACL_ART_HOME) + the AOSP host toolchain + a built bionic
    # ($AOSP, for the linker/sysroot/stub); SKIPs (exit 77) when absent. See docs/ndk-scope.md.
    if framework_android_jar != ''
      ndk_apk = custom_target(
        'ndk-apk',
        input : files('../fixtures/make_ndk_apk.py'),
        output : 'ndk.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-ndk-art',
        py3,
        args : [files('run_ndk_art_test.py'), acl_exe,
                meson.project_source_root(), framework_android_jar, ndk_apk],
        depends : [acl_exe, ndk_apk],
        timeout : 300,
      )
      # The multi-thread boundary: only the thread that bootstrapped the bionic linker has a
      # bionic TCB, so a native called off the main thread must be refused BY NAME rather than
      # run on another thread's TCB and return with its glibc %fs (which detaches the caller from
      # its own TLS — ART then dies later in the GC). Same fixture, ACL_NDK_TEST_WORKER=1.
      test(
        'run-apk-ndk-art-worker',
        py3,
        args : [files('run_ndk_art_test.py'), acl_exe,
                meson.project_source_root(), framework_android_jar, ndk_apk, '--worker'],
        depends : [acl_exe, ndk_apk],
        timeout : 300,
      )
      # ANativeWindow producer end-to-end ([[flutter-texture-bridge]] M3c): a bionic-ABI .so drives
      # one frame through the real NDK ANativeWindow C ABI (fromSurface -> lock -> unlockAndPost)
      # into an acl android.media.ImageReader, and the consumer acquires it. Runs the app through
      # run-apk.sh (the REAL vendored framework is needed for ImageReader), so it SKIPs when that or
      # the bionic toolchain is absent. The producer path a MediaCodec setVideoSurface would use.
      test(
        'run-apk-anw-producer-art',
        py3,
        args : [files('run_anw_producer_test.py'), acl_exe,
                meson.project_source_root(), framework_android_jar],
        depends : [acl_exe],
        timeout : 300,
      )
    endif

    # P2.2 / ADR-0008: acl registers the REAL framework native floor from the host
    # libandroid_runtime (--vm=art + ACL_ART_NATIVE) and a framework native executes.
    # Needs a host ART build + the real framework DEX (m framework-minus-apex); SKIPs
    # (exit 77) when absent, so CI stays green without them.
    test(
      'run-apk-native-floor-art',
      py3,
      args : [files('run_native_floor_art_test.py'), acl_exe, meson.project_source_root()],
      depends : [acl_exe],
      timeout : 300,
    )

    # ADR-0002: the same app with NO --framework must load android.* off the staged
    # default boot classpath baked into the binary (build/framework/classes).
    test(
      'run-apk-default-framework',
      py3,
      args : [files('run_default_framework_test.py'), acl_exe, activity_apk,
              meson.project_build_root() / 'framework' / 'classes'],
      depends : [acl_exe, activity_apk, framework_shim],
      timeout : 120,
    )

    # ADR-0002 android.jar layer: an app references android.util.SparseArray (NOT a
    # shim); it must resolve from the staged android.jar stub layer. Only when
    # android.jar was found + staged (framework_android_jar); skipped otherwise.
    if framework_android_jar != ''
      stub_apk = custom_target(
        'stub-apk',
        input : files('../fixtures/make_hello_apk.py'),
        output : 'stub.apk',
        command : [py3, '@INPUT@', '--stub', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-android-jar',
        py3,
        args : [files('run_android_jar_test.py'), acl_exe, stub_apk],
        depends : [acl_exe, stub_apk],
        timeout : 120,
      )
    endif

    # Gate G2.e (automotive media path): host a MediaBrowserService (no Views).
    # media.apk's MusicService extends android.service.media.MediaBrowserService;
    # `acl run --service=...` drives onCreate + onGetRoot in-process (no Binder).
    media_apk = custom_target(
      'media-apk',
      input : files('../fixtures/make_media_apk.py'),
      output : 'media.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-media',
      py3,
      args : [files('run_media_test.py'), acl_exe, media_apk, framework_shim],
      depends : [acl_exe, media_apk, framework_shim],
      timeout : 120,
    )

    # Same media path, but the service extends the ANDROIDX MediaBrowserServiceCompat
    # (the base most real Auto media apps, e.g. Spotify, extend) — the host must
    # recognize it alongside the framework MediaBrowserService.
    media_compat_apk = custom_target(
      'media-compat-apk',
      input : files('../fixtures/make_media_compat_apk.py'),
      output : 'media_compat.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-media-compat',
      py3,
      args : [files('run_media_compat_test.py'), acl_exe, media_compat_apk, framework_shim],
      depends : [acl_exe, media_compat_apk, framework_shim],
      timeout : 120,
    )

    # Gate G1.5b (automotive CAL path): host a CarAppService (no Views). cal.apk's
    # CarApp -> Session -> Screen -> ListTemplate; `acl run --cal=...` walks it and
    # the native CAL host (hosts/cal) renders + content-limits it. Automotive only.
    if profile_automotive
      cal_apk = custom_target(
        'cal-apk',
        input : files('../fixtures/make_cal_apk.py'),
        output : 'cal.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-cal',
        py3,
        args : [files('run_cal_test.py'), acl_exe, cal_apk, framework_shim],
        depends : [acl_exe, cal_apk, framework_shim],
        timeout : 120,
      )
    endif

    # Gate G2.e (media audio): an app writes PCM via android.media.AudioTrack; the
    # acl native underside forwards it to acl::audio (counting sink here;
    # ACL_AUDIO=pipewire actually plays it on the daemon).
    audio_apk = custom_target(
      'audio-apk',
      input : files('../fixtures/make_audio_apk.py'),
      output : 'audio.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-audio',
      py3,
      args : [files('run_audio_test.py'), acl_exe, audio_apk, framework_shim],
      depends : [acl_exe, audio_apk, framework_shim],
      timeout : 120,
    )

    # PipeWire backend: the same app with ACL_AUDIO=pipewire routes PCM to the
    # PipeWire sink — streaming to the daemon if one is reachable, else falling back
    # to the null sink within a bounded budget (never hangs). Only when built with
    # libpipewire (have_pipewire); the test itself adapts to daemon presence.
    if have_pipewire
      test(
        'run-apk-audio-pipewire',
        py3,
        args : [files('run_audio_pipewire_test.py'), acl_exe, audio_apk, framework_shim],
        depends : [acl_exe, audio_apk, framework_shim],
        timeout : 120,
      )
    endif

    # Phase 2 (gfx/Canvas): gfx.apk's main() builds a custom MyView extends
    # android.view.View; v.draw(c) -> onDraw(c) fills a Bitmap-backed Canvas blue and
    # draws a red rect. The Canvas/Bitmap natives raster into acl::gfx::Surface and
    # aclDescribe() dumps sample pixels — verifiable headlessly (no compositor).
    gfx_apk = custom_target(
      'gfx-apk',
      input : files('../fixtures/make_gfx_apk.py'),
      output : 'gfx.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-gfx',
      py3,
      args : [files('run_gfx_test.py'), acl_exe, gfx_apk, framework_shim],
      depends : [acl_exe, gfx_apk, framework_shim],
      timeout : 120,
    )

    # Phase 2 (View traversal): view.apk's launcher MainActivity.onCreate does
    # setContentView(new Panel(this)); Panel extends ViewGroup holding two Tiles
    # (custom Views). After the launch lifecycle the ViewRoot driver (jvm_vm
    # render_content_view) runs measure -> layout -> draw over the tree into a
    # display-sized Bitmap: the top half rasters blue, the bottom half red —
    # verifiable headlessly. Proves View(Context) + a real measure/layout/draw pass.
    view_apk = custom_target(
      'view-apk',
      input : files('../fixtures/make_view_apk.py'),
      output : 'view.apk',
      command : [py3, '@INPUT@', '@OUTPUT@'],
      depend_files : files('../fixtures/make_axml.py'),
    )
    test(
      'run-apk-view',
      py3,
      args : [files('run_view_test.py'), acl_exe, view_apk, framework_shim],
      depends : [acl_exe, view_apk, framework_shim],
      timeout : 120,
    )

    # Tier C (full vertical): the same app, but `--present` shows its rendered
    # content view in a real Wayland window. run_view_present_test.py launches a
    # headless Weston and asserts acl reports the frame was shown. Only when the
    # real wcs presenter is built and Weston is available; SKIPs otherwise.
    weston_prog = find_program('weston', required : false)
    if have_wcs and weston_prog.found()
      test(
        'run-apk-view-present',
        py3,
        args : [files('run_view_present_test.py'), acl_exe, view_apk, framework_shim],
        depends : [acl_exe, view_apk, framework_shim],
        timeout : 120,
      )

      # Tier C (continuous, JVM-driven): anim.apk's AnimView implements
      # Choreographer.FrameCallback and re-posts itself each frame; `acl run
      # --present-frames=N` runs the Choreographer loop so the app redraws a moving
      # band and N distinct frames are presented to a headless Weston.
      anim_apk = custom_target(
        'anim-apk',
        input : files('../fixtures/make_anim_apk.py'),
        output : 'anim.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-anim-present',
        py3,
        args : [files('run_anim_present_test.py'), acl_exe, anim_apk, framework_shim],
        depends : [acl_exe, anim_apk, framework_shim],
        timeout : 120,
      )
    endif

    # Tier C (input): touch.apk's TouchView.onTouchEvent logs + repaints. The wm/
    # test compositor (close_compositor, built under src/wm when wayland-server +
    # wayland-scanner are present) injects a pointer tap, which the ViewRoot driver
    # turns into a MotionEvent and dispatches into the view. Uses the compositor,
    # not Weston (headless Weston has no input to inject).
    if have_wcs and wayland_server_dep.found() and find_program('wayland-scanner', required : false).found()
      touch_apk = custom_target(
        'touch-apk',
        input : files('../fixtures/make_touch_apk.py'),
        output : 'touch.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-touch-present',
        py3,
        args : [files('run_touch_test.py'), acl_exe, touch_apk, framework_shim,
                close_compositor],
        depends : [acl_exe, touch_apk, framework_shim, close_compositor],
        timeout : 120,
      )

      # Tier C (keyboard): key.apk's KeyView.onKeyDown logs. The same test compositor
      # advertises a keyboard and injects a key press (KEY_A), which the ViewRoot
      # driver turns into a KeyEvent (evdev->Android keycode) and dispatches.
      key_apk = custom_target(
        'key-apk',
        input : files('../fixtures/make_key_apk.py'),
        output : 'key.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-key-present',
        py3,
        args : [files('run_key_test.py'), acl_exe, key_apk, framework_shim,
                close_compositor],
        depends : [acl_exe, key_apk, framework_shim, close_compositor],
        timeout : 120,
      )
      # Key repeat: the compositor holds KEY_A with a repeat rate; the presenter's
      # loop polls the wcs KeyboardHandler repeat timerfd, so onKeyDown fires >1x.
      test(
        'run-apk-key-repeat',
        py3,
        args : [files('run_key_repeat_test.py'), acl_exe, key_apk, framework_shim,
                close_compositor],
        depends : [acl_exe, key_apk, framework_shim, close_compositor],
        timeout : 120,
      )

      # Hit-testing: hit.apk's content view is a Panel (ViewGroup) with two stacked
      # Tiles. The compositor taps at (100,50) -> ViewGroup.dispatchTouchEvent must
      # route it to the top tile (which logs "top"), not the bottom one.
      hit_apk = custom_target(
        'hit-apk',
        input : files('../fixtures/make_hit_apk.py'),
        output : 'hit.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-hit-present',
        py3,
        args : [files('run_hit_test.py'), acl_exe, hit_apk, framework_shim,
                close_compositor],
        depends : [acl_exe, hit_apk, framework_shim, close_compositor],
        timeout : 120,
      )

      # IME: ime.apk's ImeView returns an InputConnection, so the presenter enables
      # text-input-v3; the compositor (acting as the IME) commits "hello", which the
      # driver routes to InputConnection.commitText.
      ime_apk = custom_target(
        'ime-apk',
        input : files('../fixtures/make_ime_apk.py'),
        output : 'ime.apk',
        command : [py3, '@INPUT@', '@OUTPUT@'],
        depend_files : files('../fixtures/make_axml.py'),
      )
      test(
        'run-apk-ime-present',
        py3,
        args : [files('run_ime_test.py'), acl_exe, ime_apk, framework_shim,
                close_compositor],
        depends : [acl_exe, ime_apk, framework_shim, close_compositor],
        timeout : 120,
      )

      # Display metrics: the compositor advertises a wl_output whose mode is 1280x720;
      # acl must query it before onCreate and seed android.view.Display, so the
      # app's getWindowManager()/getDefaultDisplay() reports 1280x720 (not the 1080p
      # default). Reuses touch.apk (any Activity queries the display on launch).
      test(
        'run-apk-display',
        py3,
        args : [files('run_display_test.py'), acl_exe, touch_apk, framework_shim,
                close_compositor],
        depends : [acl_exe, touch_apk, framework_shim, close_compositor],
        timeout : 120,
      )
    endif
  else
    message('javac not found; skipping run-apk-framework integration test.')
  endif
else
  message('python3/JDK not both available; skipping run-apk integration test.')
endif
