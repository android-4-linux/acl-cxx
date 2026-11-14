# SPDX-License-Identifier: Apache-2.0
# Provenance: original build configuration. No GPL-3.0 sources.
project(
  'a4l',
  'cpp',
  'c',   # only for scanner-generated protocol glue in the wm/ test compositor
  version : '0.0.0',
  license : 'Apache-2.0',
  meson_version : '>= 1.3.0',
  default_options : [
    'cpp_std=c++23',
    'warning_level=2',
    'werror=false',
    'default_library=static',
    'b_ndebug=if-release',
  ],
)

cxx = meson.get_compiler('cpp')

# --- Options ------------------------------------------------------------------
profile_automotive = get_option('profile_automotive')
build_tests        = get_option('tests')

# --- Optional OpenJDK (JVM) backend for runtime/ (ADR-0001 fallback) ----------
# Detect a JDK (JNI headers + libjvm). Meson's built-in `jni` dependency is
# unreliable across distros, so probe java.home directly. When no JDK is present
# the Jvm backend compiles out and create_vm(Jvm) reports Unavailable.
have_jni = false
jni_dep = declare_dependency()       # jni.h headers only (no libjvm link)
jvm_link_dep = declare_dependency()  # actual -ljvm link, for standalone JVM tools only
java_home = ''
java_prog = find_program('java', required : false)
if java_prog.found()
  jprops = run_command(java_prog, '-XshowSettings:properties', '-version',
                       check : false).stderr()
  parts = jprops.split('java.home = ')
  if parts.length() > 1
    java_home = parts[1].split('\n')[0].strip()
  endif
endif
jvm_libjvm_path = ''
if java_home != '' and import('fs').exists(java_home / 'include' / 'jni.h')
  jvm_so_dir = java_home / 'lib' / 'server'
  if import('fs').exists(jvm_so_dir / 'libjvm.so')
    # Headers only — do NOT link libjvm into the binary. The JVM fallback backend
    # (--vm=jvm) dlopen()s libjvm.so at runtime with RTLD_LOCAL, so the host HotSpot's
    # JVM_* symbols never enter the global scope. Linking it here (DT_NEEDED) loaded it
    # into every run, including --vm=art, where AOSP's libopenjdk.so then bound JVM_*
    # (e.g. Runtime.maxMemory -> JVM_MaxMemory) to the host HotSpot instead of AOSP's
    # libopenjdkjvm.so and crashed calling into an uninitialized HotSpot.
    jni_dep = declare_dependency(
      compile_args : ['-I' + java_home / 'include',
                      '-I' + java_home / 'include' / 'linux'],
    )
    # The direct -ljvm link is reserved for standalone JVM-only tools (e.g. acl-ndk-probe
    # --realvm). The acl binary must NOT use this — see the RTLD_LOCAL comment above.
    jvm_link_dep = declare_dependency(
      dependencies : cxx.find_library('jvm', dirs : [jvm_so_dir], required : false),
      link_args : ['-Wl,-rpath,' + jvm_so_dir],
    )
    jvm_libjvm_path = jvm_so_dir / 'libjvm.so'
    have_jni = true
    message('JVM runtime backend enabled: OpenJDK at ' + java_home)
  endif
endif

# audio/ backend: PipeWire when present (the AudioTrack sink actually plays);
# otherwise audio/ builds with only the counting/null sink (headless, deterministic).
pw_dep = dependency('libpipewire-0.3', required : false)
have_pipewire = pw_dep.found()

# FFmpeg (LGPL, dynamic link — permitted per plan.md license policy): decodes SoundPool
# samples (OGG/WAV/MP3/…) to PCM. Built only when all four libs are present. We take only the
# HEADERS (compile args), not the link: SoundPool dlopen()s the libs with RTLD_LOCAL at runtime so
# FFmpeg's transitive system OpenSSL never enters the global symbol scope (it would otherwise
# interpose on AOSP conscrypt's BoringSSL and crash NativeCrypto.<clinit>).
ff_full = [
  dependency('libavcodec', required : false),
  dependency('libavformat', required : false),
  dependency('libavutil', required : false),
  dependency('libswresample', required : false),
]
have_ffmpeg = ff_full[0].found() and ff_full[1].found() and ff_full[2].found() and ff_full[3].found()
ff_dep = []
if have_ffmpeg
  foreach d : ff_full
    ff_dep += d.partial_dependency(compile_args : true, includes : true)
  endforeach
endif

# wm/ windowing backend: wayland-cxx-scanner (wcs) CRTP bindings over wayland-client
# (plan.md Addendum A02). wcs is consumed as a prebuilt artifact: its scanner tool
# generates C++ protocol headers and its header-only wl/ framework provides the CRTP
# proxy classes + xdg-shell tables. When wcs + wayland-client + wayland-protocols are
# all present, wm/ builds the real Wayland shm presenter; otherwise wm/ is a stub and
# the Tier-C presentation test is skipped.
have_wcs = false
wcs_scanner = disabler()
wcs_dep = declare_dependency()   # empty no-op placeholder (wl/ header include added below)
wayland_dep = dependency('wayland-client', required : false)
wayland_cursor_dep = dependency('wayland-cursor', required : false)  # wl::CursorManager (set_cursor)
wayland_server_dep = dependency('wayland-server', required : false)  # test compositor only
xkbcommon_dep = dependency('xkbcommon', required : false)  # wcs KeyboardHandler (keymap + keysyms)
wl_scanner_dep = dependency('wayland-scanner', required : false)
wp_dep = dependency('wayland-protocols', required : false)
fs = import('fs')
# wcs is resolved in three steps, first hit wins:
#   1. -Dwcs_root=<path>                      an out-of-tree checkout
#   2. ../wayland-cxx-scanner                 a sibling checkout
#   3. subprojects/wayland-cxx-scanner.wrap   the pinned wrap, built by Meson
# 1 and 2 are the wcs-developer paths (edit wcs, rebuild it, rebuild acl against it) and
# need a scanner binary already built at <root>/build/src/wayland-cxx-scanner. 3 is what
# every other checkout gets and needs nothing on the machine beyond the wayland, wayland-
# protocols and xkbcommon dev packages — Meson clones the pin and builds the scanner.
wcs_root = get_option('wcs_root')
wcs_origin = ''
wcs_include = ''
if wcs_root == ''
  cand = meson.project_source_root() / '..' / 'wayland-cxx-scanner'
  if fs.exists(cand / 'include' / 'wl' / 'proxy_impl.hpp')
    wcs_root = cand
  endif
endif
if wayland_dep.found() and wl_scanner_dep.found() and wp_dep.found() and xkbcommon_dep.found()
  if wcs_root != ''
    wcs_scanner_bin = wcs_root / 'build' / 'src' / 'wayland-cxx-scanner'
    if fs.exists(wcs_scanner_bin) and fs.exists(wcs_root / 'include' / 'wl' / 'proxy_impl.hpp')
      wcs_scanner = find_program(wcs_scanner_bin, required : false)
      if wcs_scanner.found()
        have_wcs = true
        wcs_include = wcs_root / 'include'
        wcs_origin = wcs_root
      endif
    else
      warning('wcs checkout at ' + wcs_root
        + ' has no built scanner (build/src/wayland-cxx-scanner); using the pinned wrap instead')
    endif
  endif
  if not have_wcs
    # required : false so an offline setup, or one run with --wrap-mode=nodownload, still
    # configures: wm/ falls back to the stub presenter instead of failing the build.
    wcs_sp = subproject(
      'wayland-cxx-scanner',
      required : false,
      default_options : [
        'tests=false', 'examples=false', 'install_tool=false',
        'ime_backend=text-input-v3',  # acl selects the facade itself (src/wm/meson.build)
        'werror=false',               # an upstream warning must not fail a consumer build
      ],
    )
    if wcs_sp.found()
      wcs_scanner = wcs_sp.get_variable('scanner_exe')
      have_wcs = true
      wcs_include = meson.project_source_root() / 'subprojects' / 'wayland-cxx-scanner' / 'include'
      wcs_origin = 'subprojects/wayland-cxx-scanner (pinned wrap)'
    endif
  endif
endif
if have_wcs
  # wcs's wl/ framework is header-only; pass its include dir as a -I compile arg (like
  # jni_dep for the external JDK) — an absolute out-of-tree path can't go through
  # include_directories().
  wcs_dep = declare_dependency(compile_args : ['-I' + wcs_include])
  wl_proto_dir = wp_dep.get_variable('pkgdatadir')
  wl_core_xml = wl_scanner_dep.get_variable('pkgdatadir') / 'wayland.xml'
  xdg_shell_xml = wl_proto_dir / 'stable' / 'xdg-shell' / 'xdg-shell.xml'
  presentation_time_xml = wl_proto_dir / 'stable' / 'presentation-time' / 'presentation-time.xml'
  linux_dmabuf_xml = wl_proto_dir / 'unstable' / 'linux-dmabuf' / 'linux-dmabuf-unstable-v1.xml'
  text_input_v3_xml = wl_proto_dir / 'unstable' / 'text-input' / 'text-input-unstable-v3.xml'
  fractional_scale_xml = wl_proto_dir / 'staging' / 'fractional-scale' / 'fractional-scale-v1.xml'
  viewporter_xml = wl_proto_dir / 'stable' / 'viewporter' / 'viewporter.xml'
  message('wm/ Wayland presenter enabled: wcs at ' + wcs_origin)
endif

# ADR-4 / A02.2: the single pinned Skia build, shared with wcs (no ODR duplication),
# consumed as prebuilt archives from a prefix (lib/*.a + include/core/...), the same way
# wcs's examples do. skia_prefix option, else auto-detect the pinned standalone install.
have_skia = false
skia_dep = declare_dependency()   # empty no-op placeholder
skia_prefix = get_option('skia_prefix')
if skia_prefix == ''
  # No vendored Skia and no pkg-config for it, so the prefix is searched for. Order:
  # $ACL_SKIA_PREFIX, then the conventional install locations. Build one with wcs's
  # scripts/build-skia.sh (chrome/m150 — the shared pin, see third_party/skia/METADATA).
  _env_skia = run_command('sh', '-c', 'printf %s "${ACL_SKIA_PREFIX:-}"', check : false).stdout()
  _home = run_command('sh', '-c', 'printf %s "${HOME:-}"', check : false).stdout()
  _skia_cands = []
  if _env_skia != ''
    _skia_cands += _env_skia
  endif
  if _home != ''
    _skia_cands += [_home / '.local' / 'skia-m150', _home / '.local' / 'skia']
  endif
  _skia_cands += [
    meson.project_source_root() / '..' / 'skia-m150',
    '/opt/skia-m150',
    '/usr/local/skia-m150',
  ]
  foreach cand : _skia_cands
    if skia_prefix == '' and fs.exists(cand / 'include' / 'core' / 'SkCanvas.h')
      skia_prefix = cand
    endif
  endforeach
endif
if skia_prefix != '' and fs.exists(skia_prefix / 'include' / 'core' / 'SkCanvas.h') and fs.exists(skia_prefix / 'lib')
  cxx = meson.get_compiler('cpp')
  # is_official_build Skia keeps bundled third-party in separate archives with circular
  # refs; link every .a in one --start-group so archive order does not matter.
  _ls = run_command('sh', '-c', 'ls "' + skia_prefix / 'lib' + '"/*.a 2>/dev/null', check : false)
  _archives = _ls.stdout().strip().split('\n')
  if _archives.length() > 0 and _archives[0] != ''
    _linkargs = ['-Wl,--start-group']
    foreach a : _archives
      if a != ''
        _linkargs += a
      endif
    endforeach
    _linkargs += ['-Wl,--end-group']
    _sysdeps = [dependency('threads')]
    foreach s : ['dl', 'freetype2', 'fontconfig']
      _d = (s == 'dl') ? cxx.find_library('dl', required : false) : dependency(s, required : false)
      if _d.found()
        _sysdeps += _d
      endif
    endforeach
    # Skia headers reference each other as "include/core/..."; -isystem the prefix root
    # keeps them out of -Werror.
    skia_dep = declare_dependency(
      compile_args : ['-isystem', skia_prefix],
      link_args : _linkargs,
      dependencies : _sysdeps,
    )
    have_skia = true
    message('gfx/ Skia backend enabled: pinned Skia at ' + skia_prefix)
  endif
endif

# gfx/ GPU present path (P2.4): Skia's Ganesh Vulkan backend rendering into dma-buf-
# exported VkImages, patterned after wcs's skia-vulkan-dmabuf example (ADR-4). The pinned
# Skia is built skia_use_vulkan=true, so this needs the system Vulkan + vulkan-hpp, plus
# libdrm for the dma-buf format/modifier constants (drm_fourcc.h: DRM_FORMAT_XRGB8888,
# DRM_FORMAT_MOD_LINEAR) the V2 export path names. When absent, gfx/ is the software/
# CPU-Skia rasterizer only. (drm_syncobj explicit sync is a later, separately-gated add.)
# drm-cxx (MIT, sibling checkout via subprojects/drm-cxx symlink): the KMS-direct scanout backend for
# the split-process present sink (wm/present_sink DrmScanout, docs/split-process-art.md S2) — the
# appliance/automotive path (no compositor). Optional: when absent, the sink stays Null. Build its
# heavy/irrelevant features off.
drm_cxx_dep = dependency('drm-cxx',
  fallback : ['drm-cxx', 'drm_cxx_dep'],
  required : false,
  default_options : ['tests=false', 'vulkan=false', 'examples=false', 'benchmarks=false'])
have_drm_cxx = drm_cxx_dep.found()
if have_drm_cxx
  message('wm/ drm-cxx KMS scanout present sink enabled')
endif

have_vulkan = false
vulkan_dep = declare_dependency()
libdrm_dep = declare_dependency()
if have_skia
  vulkan_dep = dependency('vulkan', required : false)
  libdrm_dep = dependency('libdrm', required : false)  # drm_fourcc.h format/modifier fourccs
  # vulkan-hpp (vulkan/vulkan.hpp) rides on the same package but isn't always installed.
  if vulkan_dep.found() and libdrm_dep.found() and cxx.has_header('vulkan/vulkan.hpp', dependencies : vulkan_dep)
    have_vulkan = true
    message('gfx/ Ganesh Vulkan present path enabled (vulkan + vulkan-hpp + libdrm)')
  endif
endif

# The host GL device (gfx/gl_egl.cpp): headless GPU OpenGL ES on a DRM render node via
# gbm, the native underside of an app's GLSurfaceView (com.google.android.gles_jni.
# EGLImpl + android.opengl.GLES20 run on host Mesa). Needs EGL + GLESv2 + gbm + libdrm
# (drm_fourcc.h). When any is absent the GLSurfaceView path is compiled out (apps that
# need it hit the same UnsatisfiedLinkError as before, just reported).
have_gl = false
egl_dep = declare_dependency()
glesv2_dep = declare_dependency()
gbm_dep = declare_dependency()
wl_egl_dep = declare_dependency()
gl_libdrm_dep = dependency('libdrm', required : false)
if gl_libdrm_dep.found()
  egl_dep = dependency('egl', required : false)
  glesv2_dep = dependency('glesv2', required : false)
  gbm_dep = dependency('gbm', required : false)
  wl_egl_dep = dependency('wayland-egl', required : false)  # wl_egl_window (Wayland backend)
  if egl_dep.found() and glesv2_dep.found() and gbm_dep.found() and wl_egl_dep.found()
    have_gl = true
    message('gfx/ host GL device enabled (EGL + GLESv2 + gbm + wayland-egl — GLSurfaceView path)')
  endif
endif

# svc/net connectivity backend: a host D-Bus daemon (systemd-networkd on desktop,
# ConnMan on device, NetworkManager elsewhere) is translated to android.net.
# ConnectivityManager state. sdbus-c++ (LGPL-2.1, dynamically linked = policy-OK)
# is the D-Bus client. When absent, svc/net is the Null backend (offline default).
sdbus_dep = dependency('sdbus-c++', required : false)
have_sdbus = sdbus_dep.found()
net_backend = get_option('net_backend')
if have_sdbus
  message('svc/net D-Bus connectivity backend enabled (sdbus-c++ ' + sdbus_dep.version() + ', mode=' + net_backend + ')')
endif

# svc/keystore: android.security.keystore AndroidKeyStore over the freedesktop Secret Service
# (libsecret -> gnome-keyring / KWallet, LGPL-2.1 dynamic = policy-OK). Key MATERIAL persists in the
# login keyring (encrypted at rest, keyring-unlock as the user-auth gate); the crypto itself runs
# in-process via conscrypt. GObject/glib based (not sdbus). Absent -> in-memory Null backend.
secret_dep = dependency('libsecret-1', required : false)
have_secret = secret_dep.found()
if have_secret
  message('svc/keystore Secret Service backend enabled (libsecret ' + secret_dep.version() + ')')
endif

# pkg/ APK signature verification (ADR-0009 C1): OpenSSL's libcrypto (Apache-2.0 from 3.0) supplies
# SHA-2, RSA, ECDSA and X.509 parsing. Absent -> the verifier reports "unavailable" and checks
# nothing, which `--verify=require` treats as a refusal.
crypto_dep = dependency('libcrypto', version : '>= 3.0', required : false)
have_libcrypto = crypto_dep.found()
if have_libcrypto
  message('pkg APK signature verification enabled (libcrypto ' + crypto_dep.version() + ')')
endif

conf = configuration_data()
conf.set('ACL_VERSION', '"' + meson.project_version() + '"')
conf.set10('ACL_PROFILE_AUTOMOTIVE', profile_automotive)
conf.set10('ACL_HAVE_JNI', have_jni)
conf.set10('ACL_HAVE_PIPEWIRE', have_pipewire)
conf.set10('ACL_HAVE_DRM_CXX', have_drm_cxx)
conf.set10('ACL_HAVE_FFMPEG', have_ffmpeg)
conf.set10('ACL_HAVE_WCS', have_wcs)
conf.set10('ACL_HAVE_SKIA', have_skia)
conf.set10('ACL_HAVE_VULKAN', have_vulkan)
conf.set10('ACL_HAVE_GL', have_gl)
conf.set10('ACL_HAVE_SDBUS', have_sdbus)
# The GLSurfaceView present path (jvm_vm.cpp, media_codec_jni.cpp) calls wm::present_gl_loop,
# wm::active_video_presenter and create_gl_session_core, which exist only with the wcs
# presenter and the Ganesh Vulkan device. Host GL alone is not enough to compile it.
conf.set10('ACL_GL_PRESENT', have_gl and have_vulkan and have_wcs)
conf.set10('ACL_HAVE_SECRET', have_secret)
conf.set10('ACL_HAVE_LIBCRYPTO', have_libcrypto)
# Connectivity backend build mode: auto (compile all, probe the bus) | networkd |
# networkmanager | connman | null. Backends whose macro is 0 are compiled out.
conf.set10('ACL_NET_NETWORKD', have_sdbus and (net_backend == 'auto' or net_backend == 'networkd'))
conf.set10('ACL_NET_NETWORKMANAGER', have_sdbus and (net_backend == 'auto' or net_backend == 'networkmanager'))
conf.set10('ACL_NET_CONNMAN', have_sdbus and (net_backend == 'auto' or net_backend == 'connman'))
# javac matched to the detected libjvm (same java_home), for the DEX->JVM
# converter. Empty when no JDK; the converter then needs an explicit path.
conf.set('ACL_JAVAC',
         have_jni ? '"' + java_home / 'bin' / 'javac' + '"' : '""')
# Path to the host libjvm.so, dlopen()ed with RTLD_LOCAL by the --vm=jvm backend so its
# HotSpot symbols stay out of the global scope. Empty when no JDK.
conf.set('ACL_JVM_LIBJVM',
         jvm_libjvm_path != '' ? '"' + jvm_libjvm_path + '"' : '""')

# Public config header consumed across modules.
configure_file(
  output : 'acl_config.h',
  configuration : conf,
)
config_inc = include_directories('.')

# Public module headers are included as "<module>/<header>.h" off the src root.
core_inc = include_directories('src')

# --- Dependencies -------------------------------------------------------------
# Heavy vendored deps (Skia via GN, AOSP via Soong) are consumed as prebuilt
# imported artifacts and wired in per subsystem as they land — see
# third_party/*/METADATA. The AOSP APK/resource stack (libziparchive,
# libandroidfw) is not vendored yet; Phase 1 pkg/ uses a zlib-backed reader
# behind an interface until it lands.
#
# Phase 1 runtime-spine dependencies (present on a normal Linux host):
# fmt's floor is stated here rather than inherited from whatever the host happens to
# ship. Unbounded, `dependency('fmt')` resolved to 9.1.0 on the pinned runner image and
# 11.2.0 on a current Fedora -- two majors apart -- and the only reason that was safe is
# that nobody had moved either one. The floor is the oldest version this tree is actually
# built against, on every push, not the oldest that might work. There is deliberately no
# ceiling: an upper bound would be a guess about fmt 12, and a packager cannot satisfy a
# guess.
fmt_dep  = dependency('fmt', version : '>= 9.1.0')  # log facade backend (spdlog/DLT sinks come later)
zlib_dep = dependency('zlib')     # APK (zip) inflate in pkg/

# Later subsystems add their system libs here as they are implemented, e.g.:
#   wayland_client = dependency('wayland-client')

# --- Source tree --------------------------------------------------------------
# Each module subdir appends its static_library to `module_libs`, which the
# shell executable links. subdir() shares this scope, so the appends propagate.
module_libs = []
# The wm/ test compositor is built under src/wm (where the wcs/wayland-server
# deps live) but consumed by tests/ (which runs later); a disabler default keeps
# the variable defined when it isn't built. Tests using it are guarded to match.
close_compositor = disabler()
# Automotive hosts build BEFORE src so their libs (e.g. host_media_lib) exist when
# src/runtime links against them and when src/shell links module_libs. Under the
# phone profile these are absent and the runtime's uses are #if'd out.
host_media_lib = []
host_cal_lib = []
if profile_automotive
  subdir('hosts')
endif

# The android.* boot classpath (ADR-0002), staged before src/ so the shell can bake
# in its path as the default --framework.
subdir('framework')

subdir('src')

if build_tests
  subdir('tests')
endif

summary({
  'automotive profile' : profile_automotive,
  'tests'              : build_tests,
  'cpp_std'            : get_option('cpp_std'),
}, section : 'a4l')
