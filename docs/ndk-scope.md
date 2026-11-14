<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: Android NDK stable API docs (libandroid/ANativeWindow/ALooper), AOSP bionic
     (BSD) + linker docs, libhybris/Anbox (Apache-2.0, per-file license check). No GPL-3.0 sources. -->

# NDK / native libraries — scope and status
> **Binary name.** The executable was `acl` until 2026-11-13 and is **`a4l`** now. Command
> transcripts below are kept exactly as they were run; read `acl run ...` as `a4l run ...`.

`plan.md` ADR-5 stages NDK support to Phase 4. This tracks the increments.

## The chain

An APK ships native code as `lib/<abi>/*.so` (ELF, linked against **bionic** + the NDK stable
APIs). `System.loadLibrary("foo")` → the classloader's native search path → `lib<foo>.so` →
`dlopen` → its `JNI_OnLoad` runs and `RegisterNatives` binds the app's `native` methods. Running
these on a glibc/Wayland host breaks into: (1) find/extract the right ABI, (2) CPU architecture,
(3) the bionic ABI bridge, (4) the NDK trampolines. See the fuller write-up in the project notes.

## DONE — front of the chain: ABI selection + search path

- **`pkg`**: `Apk::host_abi_candidates()` gives this build's ABIs in Android's preference order
  (x86_64 → `{x86_64, x86}`, arm64 → `{arm64-v8a, armeabi-v7a, armeabi}`); `Apk::select_abi(apkAbis)`
  returns the first host-compatible ABI present, else "".
- **`a4l run`**: when the APK has native libs, extract `lib/<abi>/*.so` for the selected ABI to a
  per-app dir (`$XDG_CACHE_HOME/acl/<pkg>/lib`) and add `-Djava.library.path=<dir>` so host ART's
  `System.loadLibrary` resolves them. An ABI mismatch (e.g. an arm-only app on an x86_64 host) is
  reported, not extracted.

**Verified end-to-end** with a host-built x86_64 JNI fixture (`libacldemo.so` with a `JNI_OnLoad`
registering `answer()`; a `NativeMain` that `System.loadLibrary("acldemo")` then calls it): a4l
selects `x86_64`, extracts the lib, and the app logs `loaded libacldemo; answer()=42` — so
`loadLibrary → dlopen → JNI_OnLoad → RegisterNatives → native call` all work on host ART.

This turns the old opaque `UnsatisfiedLinkError: lib*.so: cannot open shared object file` into
either a successful load (host-compatible `.so`) or a precise ABI-mismatch report.

## NEXT — why real apps still don't load their native code

The corpus apps that hit this ship **arm-only** (Spotify `libstub.so`: arm64/armeabi) or **arm +
x86-32** (Rive `librive.so`) — no x86_64 — so on an x86_64 host they fail on **architecture** even
with the search path wired. Two hard pieces remain (Phase 4 / R-6):

1. **Bionic ABI bridge** — **DONE on x86_64** (see "The bionic bridge" below: the linker is
   bootstrapped in-process, the JNI `%fs` boundary is bridged, and a real APK's bionic
   `lib/x86_64/*.so` loads and round-trips JNI on host ART). Kept here for the arch framing:
   app `.so` are linked against **bionic**, not glibc (different libc /
   `pthread`/TLS layout / ifuncs). Build AOSP `bionic` + `linker64` for the host and load app `.so`
   in a **bionic linker namespace**, bridging only at the NDK-API boundary. The tractable target is
   an **arm64 host** (the automotive boards) where no CPU translation is needed; on x86_64 desktop
   this additionally needs arm→x86_64 translation (QEMU user / houdini-class) or x86_64-native apps.
2. **NDK trampolines** (`src/ndk/`) — resolve the `.so`'s imports to acl: `libandroid`
   (`ANativeWindow` → our dma-buf/Wayland surface, `ALooper` → the reactor, `AAssetManager` → `pkg`,
   `AChoreographer`), `liblog`, `libEGL`/`libGLESv3`/`libvulkan` → host Mesa, `libaaudio`/
   `libOpenSLES` → PipeWire, `libjnigraphics` → Skia. `NativeActivity` apps need this with no View
   tree; JNI-augmented Java apps need it plus what already works.

The front of the chain (this change) is what every later step builds on.

### GPU-from-NDK: decouple at the buffer, not the API (Vulkan external memory)

Item 2 lists `libEGL`/`libGLESv3`/`libvulkan` "→ host Mesa" as if the whole GPU API is thunked
across the bionic↔glibc line. For a **NativeActivity / native-GPU** app (a bionic Vulkan or GLES
renderer, not just JNI glue) that naive thunk is the wrong primitive — huge surface, fragile, and it
drags the app's driver state through a foreign libc. The correct decoupling is at the **buffer**:

- Give the NDK GPU code its **own bionic Vulkan ICD** (bionic V3DV / turnip — Android's own Mesa
  build) inside its namespace. It renders with its native driver, no API thunk.
- Hand images to the host glibc side via **`VK_KHR_external_memory_fd` (dma-buf)** +
  **`VK_KHR_external_semaphore_fd` (`sync_file`/`drm_syncobj`)**. The two drivers share **buffers +
  fences, never address space** — zero-copy, explicit sync, in-process (two Vulkan instances).

This is Android's own contract, not a new interop: **`AHardwareBuffer`/gralloc + `ANativeWindow`
ARE dma-buf + external-memory interop**. So a4l implements the contract it already owns:
1. **`AHardwareBuffer_allocate` → the HOST allocator** (`gbm`/minigbm on the glibc side) → dma-buf
   fd + stride + offset + modifier in the AHB handle. Single allocator ⇒ every buffer is
   host-Mesa- and display-consumable; the app only ever *imports*, never allocates.
2. **`ANativeWindow` = a4l BufferQueue over host AHBs** (`src/ndk/`): `dequeueBuffer` → a free host
   AHB + acquire fence; `queueBuffer` → return + completion fence.
3. **Two producers import the same host AHB:** the framework (glibc ART, HWUI-equiv on host
   Skia/Vulkan — trivial, no bionic) and NDK GPU code (bionic ICD, via
   `VK_ANDROID_external_memory_android_hardware_buffer`, or — since a4l owns the AHB — the raw
   dma-buf fd + `VK_KHR_external_memory_fd`).
4. **Consumer = host glibc Mesa / drm-cxx** imports the same dma-buf for composition/scanout.
5. **Sync** via `sync_file`/`drm_syncobj` fds on both handoffs — libc-agnostic.

The only things crossing the bionic↔glibc line are the AHB handle (dma-buf fd + format/stride/
modifier ints) and sync fds — no shared libc, no Vulkan-API thunk, no copy. So `libEGL`/`libGLESv2`
still thunk (small, legacy), but the **Vulkan/Impeller path decouples at AHardwareBuffer**. This
extends a4l's "re-own `android.graphics` on host Skia+Vulkan-dmabuf" decision to the NDK GPU-app
case. Open questions: modifier agreement between the two V3DV builds (same silicon ⇒ should match);
V3DV `sync_file`/`drm_syncobj` export; whether the bionic ICD loads in the libhybris namespace
in-process or needs a bionic sidecar — and if a sidecar, *that* (the app's native GPU context,
decoupled by external memory) is the only justified process split, never the framework render path.

## What a real engine expects — Flutter, read from source (so we don't implement blind)

**MEASURED against a real Flutter APK (2026-07-16).** F-Droid's Feeel (`com.enjoyingfoss.feeel`,
versionCode 4241) ships `lib/x86_64/{libflutter.so, libapp.so, libsqlite3.so}`. The prediction below
(read from Flutter's source) matches its `libflutter.so` DT_NEEDED exactly: libc/libdl/libm +
**libandroid, libEGL, libGLESv2, liblog, libjnigraphics**. Status on a4l **as measured that
day** — superseded four days later, see *Flutter runs* below:

- **`libapp.so` — the app's Dart AOT snapshot (7.3 MB) — LOADS in-process** (no DT_NEEDED at all).
- **`libsqlite3.so` LOADS** (needs only libm/libdl/libc, which the staged sysroot has).
- **`libflutter.so` stops at `library "libandroid.so" not found`** — the trampoline rung, and now
  with an exact size rather than a guess. Of its 456 undefined symbols, the NDK surface is:
  **libandroid 19, liblog 3, libjnigraphics 3, libEGL 18, libGLESv2 105** (~148 total).
- The **whole libandroid surface it links** is small and is precisely the set predicted below:
  `AAsset_{close,getBuffer,getLength,isAllocated}`, `AAssetManager_{fromJava,open}`,
  `ALooper_{acquire,addFd,forThread,pollOnce,prepare,release,removeFd,wake}`,
  `ANativeWindow_{fromSurface,getFormat,lock,release,unlockAndPost}`.
- Note `ANativeWindow_lock`/`unlockAndPost` are linked directly: this build can drive the **software**
  surface path, not only EGL/GLES. `AChoreographer_*`/`AHardwareBuffer_*`/`ASurfaceControl_*` are
  absent from the direct imports, confirming they are `dlsym`'d lazily and tolerated missing.

### Flutter runs — the `libandroid.so` wall fell on 2026-07-20

**MEASURED on screen.** Everything the bullets above call missing was built, and APKs produced by
the Flutter tooling render. The sequence, each step a commit in this tree:

- `6a49143` — a4l's own bionic-side `libandroid.so` (`src/ndk/bionic/acl_libandroid.c`), so
  `libflutter.so` resolves its direct NEEDED set and loads. `755eb3a` added the NDK
  `AChoreographer` frame clock on top of the real `ALooper`.
- `f0b8f6d` / `b238e0e` — the app's bionic EGL/GLES forward across the `%fs` boundary to host Mesa
  and bind to a4l's GL device. For a native-GL app a4l allocates no dma-buf of its own: Mesa
  renders and commits a4l's toplevel `wl_surface` inside `eglSwapBuffers` (`src/wm/present.h`, GL
  mode), which is why Flutter never needed a `SurfaceView`.
- `f177773` — the engine builds its GL environment synchronously on its raster thread *during*
  `Activity.onCreate`, while the main thread is blocked in the engine constructor; a4l published
  its GL session only after `onCreate` returned, so `get_display` waited 10s, timed out to
  `EGL_NO_DISPLAY` and the engine aborted. The native-EGL `get_display` hook now creates the
  session in place and `nGlOpen` adopts it. This broke **every** app on a recent engine build;
  older engines deferred GL init and dodged it.
- `c961326` — the engine's Skia resolves font files against a compiled-in `/fonts/` prefix, not
  `/system/fonts`, so every default face came out empty and all Roboto text drew blank while apps
  with bundled fonts looked fine. Binding the AOSP faces at `/fonts` fixed text across the corpus.
- `ae2b49f` — Dart's BoringSSL reads trusted roots from `/system/etc/security/cacerts`, which the
  AOSP target does not ship, so every `NetworkImage` HTTPS fetch failed its certificate check.
- `7bb42e6` — the external-texture path end to end: the engine's `ImageReaderSurfaceProducer`
  imports a4l-produced dma-buf frames as `EGLImage`s and composites them as a `Texture` widget.
  Animated frames on screen, on a Flutter 3.44 app on host ART.
- `a6e1931` + `f177773` — a `video_player` app (media3/ExoPlayer → `MediaCodec` → VAAPI) decodes
  H.264 zero-copy and, once the GL session was pre-created, **plays and loops** (position advances
  and wraps).

So the "genuinely hard, missing piece" named in the upshot below — the bionic libc/libm/libdl
bridge — exists (`## The bionic bridge`), and the NDK trampolines did map onto machinery a4l
already had, as predicted.

Read from `flutter/engine/src/flutter/shell/platform/android/`. `libflutter.so` (x86_64 build,
still bionic-linked) is a concrete, representative NDK client. What it needs:

- **Direct-linked libs** (must resolve at `dlopen`): bionic `libc.so`/`libm.so`/`libdl.so` (the
  bridge), `libandroid.so`, `libEGL.so`, `libGLESv2.so`, `liblog.so`, `libjnigraphics.so`.
- **`libandroid.so` functions actually called** — direct: `ANativeWindow_fromSurface` /
  `_acquire`/`_release`/`_getWidth`/`_getHeight`; `ALooper_prepare`/`_acquire`/`_addFd`/`_pollOnce`/
  `_wake` (every engine thread runs on an ALooper over real fds); `AAssetManager_fromJava` +
  `AAsset_open/close/getLength/getBuffer`; `__android_log_print`. Resolved lazily via `dlsym` and
  *tolerant of absence*: `AChoreographer_*`, `AHardwareBuffer_*`, `ASurfaceControl/Transaction_*`,
  `ATrace_isEnabled`.
- **Rendering config — pick the easy one:** the engine defaults to Impeller (Vulkan-first on API
  ≥29, needs the AHardwareBuffer/ASurfaceControl set). Forcing **`--enable-impeller=false`** (or
  targeting API < 29) selects **Skia-GLES**, which needs only an **EGL window surface** on the
  `ANativeWindow`: `EGL_OPENGL_ES2_BIT`, `EGL_WINDOW_BIT`, RGBA8888, depth=0/stencil=0, GLES2
  context, `eglSwapBuffers`. That is the minimal render path.
- **Surface flow:** Java `Surface` → `ANativeWindow_fromSurface` → `eglCreateWindowSurface` →
  GLES2 render → `eglSwapBuffers`. So the one load-bearing shim is **`ANativeWindow` backed by an
  a4l Wayland/dma-buf surface** (`wm/`+`gfx/` already produce exactly this), with EGL/GLES routed
  to host **Mesa**. **Vsync can be driven entirely from Java** (`FlutterJNI.nativeOnVsync`), so
  `AChoreographer` is optional.
- **Bring-up an embedder drives:** `FlutterLoader.startInitialization` → `loadLibrary` →
  `nativeInit` (ICU is a blob inside `libflutter.so`, located via `--icu-native-lib-path`; AOT via
  `--aot-shared-library-name=libapp.so`; JIT via `kernel_blob.bin`) → `attachToNative` →
  `runBundleAndSnapshotFromLibrary` → `nativeSurfaceCreated(Surface)` → `nativeSetViewportMetrics`
  → frames. The JNI surface is ~40 `FlutterJNI` natives (inventory in the research notes).

**Upshot for acl:** the NDK trampolines map almost entirely onto machinery that already exists —
`ANativeWindow` → `wm/`+`gfx/` dma-buf/Wayland surface, `ALooper` → the reactor, EGL/GLES → host
Mesa, `liblog` → `log/`. The genuinely hard, missing piece is the **bionic libc/libm/libdl bridge**
so the `.so` links at all; the trampolines are then a well-scoped surface, not a guess.

### Vsync — reuse a4l's wp_presentation source (per ivi-homescreen)

Reference: ivi-homescreen `shell/vsync/wayland_vsync_provider.cc` + `ivsync_provider.cc` (the
canonical Flutter-on-Wayland embedder, Apache-2.0). Its model, and the mapping to acl:

- **The contract**: Flutter hands out an opaque *baton* (embedder API) / *cookie* (Android
  `FlutterJNI.asyncWaitForVsync(long)`); the embedder returns **exactly one** vsync per baton —
  `FlutterEngineOnVsync(engine, baton, frame_start, frame_target)` / `FlutterJNI.onVsync(start,
  target, cookie)` → `nativeOnVsync` — **on the platform task runner** (Flutter rejects vsync from
  any other thread).
- **The wp_presentation association**: mint a `wp_presentation_feedback` on the surface *before*
  each `wl_surface.commit`/`eglSwapBuffers` (`RequestFeedback`); on the compositor's `presented`
  event, take the scanout timestamp and the `refresh` period and deliver **`frame_start =
  present_ns`, `frame_target = present_ns + refresh_ns`** (`PostOnVsync` → `OnVsync(engine, baton,
  now_ns, now_ns + period_ns)`). Refresh defaults to 60 Hz (16.67 ms) until the first `presented`,
  clamped to a 10 Hz floor.
- **Park vs drain**: if a present is already in flight (feedback pending) → **park** the baton, the
  `presented` event returns it; if the pipeline is idle → **drain inline** on the wall clock so
  Flutter is not stalled. `discarded` still returns the baton (wall clock).
- **Clock gate**: only usable when `wp_presentation.clock_id == CLOCK_MONOTONIC` (matches
  `FlutterEngineGetCurrentTime`); otherwise leave vsync unset and let Flutter self-schedule.

**a4l already has this exact source.** The Looper-driven present (`PresentDriver` +
`acl_deliver_vsync`, clocked by `wp_presentation` `OnPresented → present_ns + refresh`) is the same
mechanism ivi-homescreen's provider implements. On the **Android** engine path, vsync rides Java
`android.view.Choreographer` → `DisplayEventReceiver` — which a4l **already drives** from that same
wp_presentation source. So if the NDK trampoline **omits `AChoreographer_*`** (the research shows
the engine then falls back to the Java `nativeOnVsync` path), Flutter's vsync flows through a4l's
existing Choreographer/`acl_deliver_vsync` machinery for free — matching ivi-homescreen's
`frame_start = present_ns`, `frame_target = present_ns + refresh` timing. Vsync is **reuse, not new
work**; only the timestamps/thread-affinity above must be honored.

## The bionic bridge — validated foundation + the embedding mechanism

**Stage 0 — PROVEN.** A bionic x86_64 exe, run via the bionic `linker64` with the vendored bionic
runtime, `dlopen`'d a bionic `.so` and called it → `answer()=42` **on this glibc host**. So the
runtime + linker are sound; bionic code runs here. Recipe: NDK clang `--target=x86_64-linux-android24`
for the `.so`/exe; `m linker` → `…/apex/com.android.runtime/bin/linker64`; the real 64-bit bionic libs
are `out/soong/.intermediates/bionic/{libc,libm,libdl}/…/**android_x86_64_shared**/*.so` +
`linker/ld-android/android_x86_64_shared/ld-android.so` (**not** `android_x86_x86_64_shared`, which is
32-bit); run `LD_LIBRARY_PATH=<bionic-libs> ./linker64 <ABSOLUTE-path-exe>` (the missing
`/linkerconfig/ld.config.txt` warning is harmless).

**Stage 0b — the in-process embedding — PROVEN (2026-07-16).** `tools/ndk-loader-spike/` (run
`build.sh`) does it end to end on the x86_64 host: `SPIKE: nativeAnswer() = 42 (PASS)`, deterministic.
A **glibc** process maps `linker64`, calls `__dl___linker_init` *directly as a function* with a
synthetic `KernelArgumentBlock` (standalone mode: `AT_ENTRY` = the linker's `_start`, a minimal
bionic stub exe as `argv[1]`), then `__loader_dlopen`s a bionic `.so` — resolving its whole dep graph
(bionic `libc`, `liblog`→`libc++`, `libdl`) — `__loader_dlsym`s the symbol, and **calls it** (its
`__android_log_print` runs, no crash) → 42. So bionic app native code loads and runs *inside* a glibc
process; the host NDK premise is de-risked. Key facts that made it work (all verified in the spike):
- **The TLS crux is real but bounded.** `__linker_init` sets `%fs` to bionic's TCB; we `ARCH_GET_FS`
  before and restore glibc's `%fs` after, and bracket every bionic call (dlopen/dlsym/the app fn) with
  the swap. `__libc_init_main_thread_late` migrates `%fs` to a **permanent mmap'd TCB**, so calling
  `__linker_init` and returning is safe (the stack `temp_tcb` isn't the lasting one). Harness is
  `-fno-stack-protector` (the canary lives at `%fs:0x28`; a mid-function swap would false-fail it).
- **No init-only mode**, so we hand the linker a throwaway bionic stub exe and call `__linker_init`
  *directly* (not via `_start`, whose `jmp *%rax` would jump into the stub) to keep control.
- **Linker input errors are fatal** (it `__linker_error`+aborts the process) — real integration must
  pre-validate + absolute-path inputs. Bionic deps resolve via `LD_LIBRARY_PATH` (no host `ld.config`).

The mechanism (map PIE → synthetic KAB → `__linker_init` → `__loader_*`) is arch-neutral in structure;
the arm64 port swaps only the TLS register (`TPIDR_EL0`), auxv, and reloc details (the target
`linker64` carries the relocs). The remaining work is integration, not feasibility.

**Rung 3 — promoted into `src/ndk/` (2026-07-16).** The spike is now a real a4l API:
`src/ndk/bionic_runtime.{h,cpp}` — `BionicRuntime` (process-singleton; the linker bootstraps once),
`BionicConfig` (paths from `ACL_BIONIC_LINKER`/`_SYSROOT`/`_STUB`, validated up front because linker
input errors are fatal), `load()`/`symbol()`, and an RAII `RunScope` that brackets bionic calls with
the `%fs` swap (the swap confined to the `-fno-stack-protector` TU). Arch-guarded: non-x86_64 returns
a clean "arm64 TLS port pending" error so cross builds stay green. Verified via `acl-ndk-probe`
(`nativeAnswer()=42 (PASS)`, logging through a4l's `log/`).

**Rung 4a — the bidirectional %fs boundary, PROVEN (2026-07-16).** `acl-ndk-probe --boundary` →
`boundary selftest = 1010 (PASS)`. A bionic `.so` under bionic `%fs` writes a bionic thread-local,
calls **back into glibc** (which writes glibc `errno` under glibc `%fs`) through an internal up-call
trampoline that restores glibc `%fs` for the callback and bionic `%fs` on return, and the bionic TLS
is **intact** afterward (the `+1000`). This is the JNI up-call/down-call mechanism in miniature —
`BionicRuntime::run_boundary_selftest` + the `-fno-stack-protector` `upcall_trampoline`. So a loaded
app `.so` can safely call ART's JNI and return. Toolchain note: android clang defaults to
**emulated-TLS** (`__emutls_get_address`, a compiler-rt symbol absent from bionic) — app/fixture
`.so` need `-fno-emulated-tls` so `__thread` uses native ELF TLS (`__tls_get_addr`, bionic-provided).

**Rung 4b — the JNIEnv/JavaVM boundary trampoline, PROVEN (2026-07-16).** `acl-ndk-probe --jni` →
`JNI_OnLoad ver=0x10006 up-calls=4 probe_tls=333 native=4200 (PASS)`. A bionic `.so`'s **real
`JNI_OnLoad`** drives `GetEnv` (VM) → `GetVersion`/`FindClass`/`RegisterNatives` (Env) — 4 up-calls,
each crossing bionic→glibc through a `%fs`-restoring thunk (`probe_tls=333` confirms glibc TLS stayed
intact across them) — and the `RegisterNatives`-bound native is then invoked via a down-call
(returns 4200). Impl: `BionicRuntime::wrap_java_vm(real_vm, real_env)` returns a wrapper `JavaVM`
whose `GetEnv` hands back a wrapper `JNIEnv`; the thunked slots run the real ART fn under a `ToGlibc`
guard. Portable across jni.h variants via `JniEnvTable`/`JniVmTable` (derived from the `functions`
member — OpenJDK names the structs `*_`, Android drops it; same ABI). **The full JNIEnv/JavaVM thunk table is now complete (2026-07-16)** — all ~232 JNIEnv slots +
the JavaVM invoke interface are `%fs`-thunked (`ToGlibc`), generated via type-family macros
(`Call*Method` ×10 types ×3 kinds ×3 forms, `Get/Set*Field` instance+static, primitive `*Array*`
families) plus the fixed-signature functions. Varargs `Call*Method`/`NewObject` forward through
their `...V` variants; `GetJavaVM`/`GetEnv` hand back the wrapper. Runtime-verified including the
riskiest path: `acl-ndk-probe --jni` now does a `CallIntMethod(20,30)` **varargs up-call** from the
registered native → `native=4250` (`4200+50`), proving `va_list`→`...V` across the boundary.

**Down-call trampoline + `load_library` — PROVEN (2026-07-16).** The reverse direction: when ART
invokes a `RegisterNatives`'d bionic native it calls under **glibc** `%fs`, but the native is bionic
code needing bionic `%fs` + the wrapped `JNIEnv`. `w_RegisterNatives` now emits, per native, a tiny
**runtime-generated machine-code trampoline** (`make_downcall`) that substitutes the wrapped env,
`wrfsbase`→bionic, calls the native, `wrfsbase`→glibc, returns — signature-agnostic for
register-argument natives (the JNI norm; stack-arg natives would need arg copying, TODO). Uses
FSGSBASE (verified available: CPU flag + kernel 7.1 + userspace `rdfsbase`). `acl-ndk-probe --jni`
now invokes the native **from glibc `%fs`, exactly as ART would**, through the trampoline →
`native=4250`: the complete realistic round-trip **glibc(ART) → down-call trampoline → bionic native
→ varargs up-call trampoline → glibc → back**. And `BionicRuntime::load_library(path, real_vm,
real_env)` packages the whole thing — dlopen the app `.so`, wrap ART's `JavaVM`, drive `JNI_OnLoad`
across the boundary — the single call the `nativeLoad` hook makes.

**End-to-end against a REAL JVM — PROVEN (2026-07-16).** `acl-ndk-probe --realvm` boots a genuine
host OpenJDK JVM (linked via `jni_dep`), bootstraps the bionic linker **in the same process** (glibc
JVM + bionic runtime coexisting), `load_library`s the bionic `.so` — driving `JNI_OnLoad` against the
**real JVM** (real `FindClass`/`GetStaticMethodID`/`RegisterNatives`) — then calls `Main.answer()`
**from Java**: the JVM's own dispatch invokes the down-call trampoline → bionic native → a
`helper(20,30)` `CallStaticIntMethod` up-call back into the JVM → `4250 (PASS)`. So the entire stack
works against a genuine VM, which is exactly what `jvm_vm.cpp` provides. The Java fixture
(`com.acl.ndktest.Main` with a native `answer()` + a `helper()`) is built by
`tools/ndk-loader-spike/build.sh` (`ACL_NDK_TEST_CLASSES`).

**`Runtime.nativeLoad` hook — WIRED (2026-07-16, gated).** `jvm_vm.cpp`
`register_ndk_loadlibrary_override` (called in the ArtVm boot sequence next to the other
`register_*_override`s) installs, **only when `ACL_BIONIC_LINKER` is set**, an override of
`java.lang.Runtime.nativeLoad(String,ClassLoader,Class)`: it configures `BionicRuntime` from
`ACL_BIONIC_*` (+ `ACL_BIONIC_APP_LIBS` for inter-lib deps), and `ndk_native_load` routes each load
by `acl::ndk::is_bionic_elf(path)` — bionic app libs → `BionicRuntime::load_library(path, artVM,
artEnv)`, a4l's own glibc supplemental libs (e.g. soundpool) → `dlopen`+`JNI_OnLoad`. Unset ⇒ no-op,
ART's own `nativeLoad` untouched (zero regression: full build + 33 tests green). `is_bionic_elf`
(DT_NEEDED `libc.so` vs glibc `libc.so.6`) validated: bionic `.so`/stub → BIONIC, `libz.so.1`/`ls` →
glibc. The mechanism it calls is the `--realvm`-proven stack.

**A REAL APK's native lib, on REAL host ART — PROVEN end-to-end (2026-07-16).** The mechanism above
is now reached the way an app reaches it, with no probe harness in the loop. `tests/integration/
run_ndk_art_test.py` (`meson test run-apk-ndk-art`; SKIPs without `ACL_ART_HOME`/AOSP) runs
`acl run ndk.apk --vm=art` on an APK carrying a **bionic-ABI `lib/x86_64/libndktest.so`** and a real
`Activity` (`tests/fixtures/art_ndk/Main.java`). The whole vertical runs: `pkg` extracts the `.so` →
`main.cpp` exports `ACL_BIONIC_APP_LIBS` → ART boots → the gated hook routes
`Main.<clinit>`'s `System.loadLibrary("ndktest")` → `is_bionic_elf` ⇒ bionic → in-process linker
dlopen + `JNI_OnLoad` across the `%fs` boundary (`RegisterNatives`) → `onCreate` calls `answer()` →
down-call trampoline → bionic native → `helper(20,30)` varargs up-call into ART → **4250**. The
asserted value proves load + `JNI_OnLoad` + down-call + up-call, not merely that a library loaded.

`tools/stage-bionic-runtime.sh [outdir] [--print-env]` stages what the loader bootstraps (linker64 +
soname-named bionic sysroot + PIE stub) and is the single definition of "a bionic runtime" — the
Stage-0b spike (`tools/ndk-loader-spike/build.sh`) now consumes it too, so spike and run path
bootstrap identical bits.

**Signal state is host state (found by the above, fixed).** The linker's init claims two pieces of
process/thread state for bionic: the crash handlers (`linker_debuggerd_init` →
`debuggerd_register_handlers`: SIGABRT/BUS/FPE/ILL/SEGV/STKFLT/SYS/TRAP + `BIONIC_SIGNAL_DEBUGGER`)
and **this thread's `sigaltstack`** (`__libc_init_main_thread_late` → `__init_alternate_signal_stack`
mmaps a guarded region and installs it). Both must stay a4l's: ART installs its own SIGSEGV handler
(implicit null checks are SEGVs it fixes up), and on host `art::Thread::TearDownAlternateSignalStack`
reads back whatever `sigaltstack` is installed and `delete[]`s it — with bionic's mmap there, **ART
shutdown `free()`d a bionic guard-page address and SIGSEGV'd** (the app had already run correctly;
the crash was in teardown). `bootstrap_x86_64` now snapshots the host's `sigaltstack` + those
sigactions across `__linker_init` and restores them, so a4l stays the crash handler and owns its own
alt stack; bionic's stay mapped but unreferenced. A bionic crash is then reported by the host —
what we want from a translation layer anyway.

**Name-resolved natives — DONE for the loading class (2026-07-16).** The other half of binding.
ART binds a native lazily by `dlsym`ing `Java_pkg_Class_method` on the handles *it* opened
(`JavaVMExt::FindCodeForNativeMethod` → `Libraries::FindNativeMethod`); a bionic app lib is opened
by the bionic linker, so ART holds no handle and that lookup can **never** succeed
(`UnsatisfiedLinkError: No implementation found for … (tried Java_com_acl_ndktest_Main_nativeAnswer
…)`). The lazy path can't be hooked either: `ClassLinker::RegisterNative` CHECKs a non-null address,
so JVMTI's `NativeMethodBind` only fires for an address ART **already** found — it can redirect a
binding, not create one. So a4l binds **eagerly** at load time (`src/ndk/native_bind.{h,cpp}`):
enumerate the loading class's declared natives by reflection, mangle each per the JNI spec (short
name, then the overload long name), `__loader_dlsym` it in the bionic lib, wrap it in a down-call
trampoline (`BionicRuntime::downcall_symbol`) and `RegisterNatives` it. `load_library` now wraps the
JavaVM unconditionally (a name-bound-only lib has no `JNI_OnLoad`, but its trampolines still need
the wrapped env).

Two subtleties worth keeping:
- **Which class?** `nativeLoad`'s `caller` argument is null exactly when it matters: libcore's
  `Runtime.loadLibrary0` passes `callerClass` only on the boot-classloader path — for an app (a
  `PathClassLoader`) it calls the 2-arg `nativeLoad(filename, loader)`, which hardcodes
  `caller=null`. So `loading_class()` recovers it from the Java stack (the first frame below
  `java.lang.*`) and resolves it through the `ClassLoader` argument with `initialize=false` (that
  class is mid-`<clinit>` — its static initializer is what called `loadLibrary`).
- **A `JNI_OnLoad` registration wins.** A lib may both export `Java_pkg_Class_m` and register a
  *different* implementation for `m`; the registration is the app's explicit choice, so name binding
  skips methods `JNI_OnLoad` already registered (`BionicRuntime::was_registered`). The repro .so
  exports a **decoy** `Java_com_acl_ndktest_Main_answer` returning 9999 to keep this honest —
  drop the guard and the end-to-end test's `answer()=4250` becomes 9999 (verified).

**Still NOT covered: natives declared in a class OTHER than the one that called `loadLibrary`.** The
canonical NDK idiom (a class loads its lib in its own static initializer and declares its natives) is
covered; a `Loader` class loading the lib for natives declared elsewhere is not, and there is no hook
for it — ART's lazy resolution is closed to us. The only complete fix in sight is handing ART a
*glibc proxy `.so`* to `dlopen` whose exported `Java_*` symbols forward into the bionic lib, which
means synthesizing an ELF at run time (bounded, but real work). Revisit when a corpus app needs it.

**The thread boundary — made explicit, not yet crossed (2026-07-16).** A bionic TCB belongs to ONE
thread (errno, `pthread_self`, pthread TLS, the DTV), and only the thread that bootstrapped the
linker has one. The down-call trampoline used to bake BOTH thread pointers in as immediates, so a
native called from an ART worker thread ran on the main thread's bionic TCB and — the real damage —
returned with the **main thread's glibc `%fs`** installed. Measured, not theorised: the native
returned 42 quite happily and the process then died inside the GC with `Check failed: self == thread`
(`concurrent_copying.cc`), because `Thread::Current()` is a TLS read and the worker was now reading
the main thread's TLS. Nothing in that abort points at the NDK bridge.

Both pointers are now per-thread: the trampoline `rdfsbase`s the **caller's own** glibc `%fs` and
stashes it on the stack (restoring a constant would detach any other caller from its TLS), and takes
the bionic side from a per-thread `t_bionic_fs` slot, read at a fixed `%fs` displacement while still
on the caller's glibc `%fs` (hence `initial-exec`: it guarantees a constant offset from the thread
pointer for every thread). A zero slot — a thread that was never attached — calls
`acl_ndk_unattached()`, which logs what happened and aborts **at the call**, rather than running on
another thread's TCB. `meson test run-apk-ndk-art-worker` pins this: the refusal must name the
reason, and `Check failed: self == thread` must NOT appear. The up-call thunks (`ToGlibc`) still use
a constant `%fs` pair, which is correct exactly because a native can only run on the (single)
attached thread; per-thread attach must revisit that too.

**Per-thread attach-as-bionic — the TCB is now synthesized, and it works (2026-07-16).**
`BionicRuntime::attach_current_thread()` implements the design below;
`acl-ndk-probe --thread` / `meson test ndk-attach` proves it on a real second thread: a
glibc-created thread gets a hand-built TCB and then runs **real bionic libc** as itself —
`nativeAnswer()` returns 42 having gone through `__android_log_print` → liblog → bionic libc
(which touches its TLS, so a wrong TCB or a wrong stack canary crashes rather than returning),
bionic's `gettid()` agrees with the kernel, and `pthread_self()` hands back that thread's **own**
`pthread_internal_t`. `RunScope` now runs bionic code as the calling thread and refuses an
unattached one by name instead of borrowing the bootstrapping thread's TCB.

**Wired into the down-call path (2026-07-16): an app's native now runs on ANY thread.** An ART
worker thread attaches itself on its first native call and the native works —
`run-apk-ndk-art-worker` asserts exactly that, plus a clean exit and the absence of
`Check failed: self == thread` (the ART corruption a borrowed TCB used to cause). Three pieces had
to become per-thread together; shipping any one alone would have re-introduced that corruption:
- **Lazy attach.** The trampoline's unattached branch calls `acl_ndk_attach_thunk`, which saves and
  restores every argument register (`rdi/rsi/rdx/rcx/r8/r9` + `xmm0-7`) around the attach — the
  trampoline is signature-agnostic and cannot know which are live. Real assembly, not emitted
  bytes: the assembler checks it, and it is the same for every native.
- **Per-thread up-calls.** `ToGlibc` no longer restores a constant glibc `%fs` (the bootstrapping
  thread's). An up-call runs under bionic `%fs`, where glibc TLS is unreachable, so the pairing is
  looked up by bionic `%fs` in an append-only table registered at attach.
- **A per-thread wrapped JNIEnv.** A JNIEnv belongs to one thread; a single global handed every
  thread the env of whichever one loaded the library. The trampoline points this thread's
  `t_wrap_env` at the real env ART just passed, before swapping.

**The trap worth naming: a glibc TLS access under bionic `%fs` is silently wrong.** `w_GetEnv` took
`&t_wrap_env` just *after* its `ToGlibc` scope closed, so the displacement resolved against bionic's
TCB and produced a garbage JNIEnv that segfaulted on first use. Keep glibc TLS reads inside the
scope that holds glibc `%fs`.

**Thread exit is handled** (2026-07-16): attach arms a glibc `pthread_key_create` destructor, which
runs on the exiting thread and unmaps its TLS block + thread struct and frees its fs-pair slot
(unregistering before the unmap, so no up-call can be routed to a TCB about to disappear). Bionic
cannot help here — it never created the thread, so it runs no exit path for it, and
`__libc_register_thread_exit_callback` only fires for its own. The fs-pair table reuses slots for
the same reason: append-only, it would exhaust under thread churn alone. `acl-ndk-probe --thread`
churns 300 attach/exit cycles (more than the table holds) and asserts the mapping count does not
grow; with the destructor disabled it aborts at 256, so the test fails when the code is wrong.

**Still leaks, and cannot be fixed from out here:** if an attached thread's native touched a
dlopen'ed module's `__thread` variable, bionic's allocator holds blocks off that thread's DTV, and
`__free_dynamic_tls` — the only thing that knows how to release them — is `__LIBC_HIDDEN__`.
Bounded by how many threads touch dynamic TLS, not by thread count. Attach also reads the TLS module
table without bionic's rwlock (taking it would mean calling bionic code from a thread that has no
TCB yet — the chicken-and-egg attach breaks); the static modules are fixed after startup, so the
race is a `dlopen` concurrent with an attach.

The design, for reference — bionic offers no attach API: itThe design, for reference — bionic offers no attach API: it
exports `pthread_create`, `__libc_get_static_tls_bounds`, `__libc_register_thread_exit_callback`,
`__tls_get_addr` — nothing that adopts an already-running (glibc-created) thread, and the
`__init_static_tls` / `__init_tcb*` / `__init_thread` helpers that build a TCB are `__LIBC_HIDDEN__`,
not exported. `pthread_create` is no use either, with or without a caller-supplied stack
(`pthread_attr_setstack` does make bionic map TLS-only): it always `clone()`s a *new* OS thread, and
the native must run on ART's own thread — the one holding `Thread::Current()` and the JNIEnv. A
donor thread's TCB can't be re-pointed at another thread either: `pthread_internal_t::tid` backs
recursive/errorcheck mutex ownership, so two threads sharing a tid break mutual exclusion. So a4l
must synthesize the TCB. Read for it: `bionic/libc/bionic/pthread_create.cpp` (`__allocate_thread`),
`__libc_init_main_thread.cpp`, `bionic_elf_tls.cpp`, `private/bionic_elf_tls.h`.

**Do NOT copy the main thread's static TLS block** (an earlier draft of this doc said to; it is
wrong). The block contains `bionic_tls` — pthread `key_data`, locale, the cleanup stack — so a copy
would silently inherit the main thread's pthread TLS and cleanup state; and the module TLS in it
holds the main thread's *current* values, where a new thread must get the pristine init images.
Build it the way bionic does instead — the pieces are all reachable:

1. **Shared globals.** The linker exports `__loader_shared_globals` (libc imports it), so the
   bootstrap can `__loader_dlsym` it for `libc_shared_globals*` → `static_tls_layout` (sizes +
   `offset_bionic_tcb`/`offset_bionic_tls`) and `tls_modules` (the module table).
2. **A fresh, zeroed static TLS block** of `layout.size()` (mmap is already zero-filled), then the
   `__init_static_tls` equivalent: for each module with `static_offset != SIZE_MAX`, memcpy its init
   image to `static_tls + static_offset`.
3. **The TCB slots** at `static_tls + layout.offset_bionic_tcb()` (x86_64 slot numbers in
   `platform/bionic/tls_defines.h`): `TLS_SLOT_SELF` = the TCB itself, `TLS_SLOT_THREAD_ID` = the
   new `pthread_internal_t`, `TLS_SLOT_STACK_GUARD` = `__stack_chk_guard` (exported), 
   `TLS_SLOT_BIONIC_TLS` = `static_tls + layout.offset_bionic_tls()`, `TLS_SLOT_DTV` = a zero DTV.
4. **The zero DTV is safe to provide ourselves** — this is the subtle one. `update_tls_dtv` chains
   the old DTV unconditionally (`new_dtv->next = old_dtv`) and `__free_dynamic_tls` walks that chain
   with bionic's own allocator, which would be a wild free of our memory. It is not, because the
   free loop is `while (dtv->generation != kTlsGenerationNone)` — it stops *at* the zero DTV. So an
   all-zero `TlsDtv` works (generation 0 == `kTlsGenerationNone`), and one process-wide instance
   suffices, exactly as bionic's own `static const TlsDtv zero_dtv` does. The first dynamic-TLS
   access then lazily builds the thread's real DTV.
5. **`pthread_internal_t`**: fresh + zeroed, `tid` = this thread's `gettid()`, `bionic_tls` = the
   block's, and stack bounds taken from the *glibc* thread (`pthread_getattr_np`) so unwinders and
   `pthread_getattr_np` on the bionic side describe the stack the code is really on.
6. Store the TCB in `t_bionic_fs`; the trampoline picks it up with no other change.

That couples us to bionic's internal `pthread_internal_t` / `bionic_tls` / `StaticTlsLayout` /
`TlsModules` layout. Keep it honest by **generating the offsets at stage time** — compile a probe
against the private headers of the *pinned* bionic in `$AOSP` (`tools/stage-bionic-runtime.sh`
already owns that pairing) and emit a header of `offsetof`s, rather than hand-copying structs that
drift. Thread exit also needs `__libc_register_thread_exit_callback` (or an explicit detach) to run
`__free_dynamic_tls` and unmap, or every attached thread leaks its block.

NEXT (to real NDK apps): (1) thread-exit cleanup for attached threads (above); (2) NDK trampoline
`.so` (`liblog`→a4l `log`, `libandroid`→`wm`) for NativeActivity/GPU apps; then **arm64** (the
mechanism is arch-neutral: swap `%fs`→`TPIDR_EL0`, redo auxv/reloc details, and note
`MIN_TLS_SLOT` is negative there, so the thread pointer is NOT the TCB address —
`StaticTlsLayout::offset_thread_pointer()` must be used).

The mechanism, pinned:
- The bionic `linker64`/`ld-android.so` export a programmatic loader — `__loader_dlopen`,
  `__loader_android_dlopen_ext`, `__loader_dlsym`, `__loader_android_create_namespace`, … (bionic
  `dlfcn.cpp`).
- But glibc **cannot** call them naively: glibc's own linker can't even map `ld-android.so` (a bionic
  `.so` whose `NEEDED` don't resolve against glibc), and `__loader_dlopen` requires the linker's
  `solist`/namespaces initialized plus a `caller_addr`.
- So the bionic linker must be **bootstrapped in-process**: map `linker64` (PIE, self-relocating),
  call `__linker_init(KernelArgumentBlock)` (bionic `linker_main.cpp:698`) with a **synthetic**
  argument block (fake `argc/argv/envp/auxv`, no real main executable), set up TLS + a default
  namespace, then `__loader_android_dlopen_ext` the app `.so` in that namespace with our trampoline
  `.so` on its search path.
- This is exactly what **libhybris** (Apache-2.0) implements (for the reverse direction — glibc apps
  using bionic GL drivers). Options: **(a)** vendor/patch libhybris's linker-loading core, or **(b)** a
  purpose-built bootstrap in `src/ndk/` (a controlled subset — a4l loads a known `.so` set, not the
  general case). Both are bionic-version-specific (target the vendored android-13 bionic) and are a
  focused multi-step build, not a small shim. The two-libc coexistence (bionic libc for app `.so` +
  glibc for a4l, bridged at the NDK/JNI boundary) is the R-6 risk to manage here.

### Stage 0b — libhybris core vs a purpose-built bootstrap (the two paths, contrasted)

They are **different philosophies**, not two ways to write the same thing:

- **libhybris = one libc, bionic calls hooked to glibc.** libhybris carries its *own fork* of the
  bionic linker (`hybris/common/<ver>/`) and a large `hooks.c` that intercepts bionic libc calls and
  routes them onto glibc — because in its world (glibc apps using bionic **GL drivers**) FILE*/`pthread_t`/
  `errno`/locale structs cross the boundary constantly, so both sides must share *one* libc's state.
- **Purpose-built = two libcs, isolated, bridged only at JNI.** This is what **Stage 0 already proved**:
  load the *real* AOSP `linker64` + real bionic `libc/libm/libdl` in their own namespace; the app `.so`
  resolves its `NEEDED` against real bionic libc (no per-function hooking). glibc (ART, Skia, Mesa) and
  bionic (app `.so`) never share libc state — the only thing crossing is the **JNI ABI** (`JNIEnv*` +
  `jobject` handles) and the **NDK ABI** (our trampoline `.so`). A JNI boundary is far narrower than a
  GL-driver boundary — no shared `FILE*`/`pthread_t` — so the hook surface collapses to nearly nothing.

| Dimension | libhybris core | Purpose-built `src/ndk/` bootstrap |
|---|---|---|
| Libc model | one (glibc), bionic **hooked** | two, **isolated** (Stage-0 proven) |
| Correctness for arbitrary app JNI | hooks tuned for **GL drivers**; an app's full bionic libc surface may exceed them | app gets **real bionic libc** → behaves as on-device |
| Version coupling | pinned to libhybris' forked linker; must track our android-13 bionic separately | uses the **AOSP tree's own** `linker64`+bionic → auto-matched to the vendored build |
| Direction fit | built for glibc→bionic-**GL**; must be re-patched for glibc-ART→bionic-**app-JNI** | built for exactly our direction |
| Dependency | vendor + patch an external tree (Apache-2.0, per-file check) | fully in-tree, no new dep |
| Time-to-first-`dlopen` | faster (proven loader) | slower (write the KAB/auxv/TLS bootstrap) |
| Ongoing ownership | track upstream + our patches | we own a small, controlled loader |

**The genuinely hard part is the same for both: TLS / thread-pointer coexistence.** Both glibc and
bionic put the thread pointer in the *same* register — `%fs` on x86_64 (`arch_prctl(ARCH_SET_FS)`),
`TPIDR_EL0` on arm64 — and each libc expects it to point at *its own* TCB. ART's threads (main + JNI)
are glibc threads, so when app bionic code on an ART thread calls a bionic libc function it reads TLS
off glibc's TCB → garbage. Resolutions (both approaches must pick one): (a) run app native code on
**dedicated bionic-TLS threads** and marshal JNI upcalls across a known seam (cleanest for the
two-libc model); (b) **swap the thread pointer** on every bionic entry/exit (what libhybris does in
spots — fragile, per-call cost); (c) lay bionic's TLS slots so they don't collide with glibc's usage
on that TP (brittle across libc versions). This is the R-6 crux and it is **arch-specific** — the
register, the set-TLS syscall, and the ELF relocation types (`R_AARCH64_*` vs `R_X86_64_*`, handled by
the target's own `linker64`) all differ per arch.

### The architecture axis — loader vs CPU-ISA (they're orthogonal)

The loader question is separate from *can the app's machine code even run here*:

- **arm64 host (the automotive boards): app `lib/arm64-v8a/*.so` is native → the loader alone is
  sufficient.** No CPU translation. This is the **tractable target** and it's the automotive critical
  path — build the arm64 `linker64`+bionic, bootstrap in-process, done.
- **x86_64 host (desktop): the corpus ships arm-only** (Spotify `libstub`, Rive `librive`) — so even a
  perfect loader maps in an **arm64** `.so` that **cannot execute** on x86_64 without a CPU translator
  (QEMU user-mode / houdini-class) — a separate, larger effort. The loader **is** sufficient on x86_64
  for the **x86_64-native** NDK apps (a Flutter `--target-platform android-x64` build, apps that ship
  `lib/x86_64/`).

**Upshot:** the bootstrap *mechanism* is largely arch-neutral in structure (map PIE linker →
`__linker_init(synthetic KAB)` → set TLS → `__loader_android_dlopen_ext`); only the TLS/auxv/reloc
details are per-arch, and the target's own `linker64` already carries the reloc handling. So the
smart target order is **arm64 first** (loader = complete story, matches the boards), and on x86_64
scope to x86_64-native `.so` until/unless a CPU-translation layer is justified. Recommendation:
**purpose-built** — Stage 0 already validated its exact substrate (real `linker64` + real bionic libc
run here), it auto-matches the vendored bionic, keeps the JNI boundary a clean two-libc seam, and
avoids owning a patched fork of someone else's linker aimed at a different direction; mine libhybris'
`hooks.c` and TP-swap code as a **reference** for the TLS crux rather than vendoring the whole tree.

## Security — signing & containment (R-10)

Native libs are **not individually code-signed** — an ELF `.so` carries no signature, and the bionic
linker verifies nothing at load (so our loader replicating bionic needs no signature logic). Integrity
comes from the **whole-APK signature**: v1 (JAR) digests every entry in `MANIFEST.MF` (covers each
`lib/<abi>/*.so`); v2/v3 sign the archive bytes as a blob. So the `.so` are vetted **exactly as much
as the APK is** — nothing separate to vet at the `.so` level.

- **Integrity is a `pkg/`-layer, one-time concern, not a per-load one.** The right design is a single
  APK v2/v3 signature verification when a4l *opens* the APK (mirroring Android install-time), after
  which every extracted `.so` is trusted by construction. a4l does this as of 2026-11-14
  (`src/pkg/apk_verify.cpp`): v2, v3 and v3.1, enforced with `--verify=require` or a pinned
  `--signer`, and by default only refusing a signature that fails. One gap: the runtime re-reads
  the APK by path, so "by construction" holds only while nobody replaces the file. The NDK loader
  itself stays signature-free.
- **The real runtime worry is containment, not signing.** Signing says *who authored* the code, not
  that it's *safe*. Once an app `.so` runs it is arbitrary native code in a4l's **single glibc
  process** — no per-app UID / SELinux / seccomp cage like on-device Android — so it can reach ART,
  Mesa, and the Wayland connection. That's an architectural sandboxing question, orthogonal to both
  signing and the loader, and decided in ADR-0009: a process boundary at the dma-buf seam, because
  a seccomp filter binds a thread and no thread here runs only app code. Not implemented. For the
  hash-pinned dev corpus the files are user-vetted, so neither blocks bring-up.

## Verify

```sh
# The gap, reproducibly (negative control): build a real bionic-ABI .so and show that a plain
# glibc dlopen CANNOT load it — glibc's ld.so can't satisfy its NEEDED bionic libc.so.
# This is WHY Stage 0b (a bionic linker bootstrapped in-process) is needed, not glibc dlopen.
tools/build-ndk-repro.sh    # -> NEEDED libc.so/liblog.so/libdl.so; "dlopen FAILED: .../libc.so: invalid ELF header"
# Stage 0 (scratchpad /tmp/acl/scratchpad/bionic0/): bionic .so runs on host via linker64
LD_LIBRARY_PATH=broot/lib64 broot/linker64 $PWD/broot/hello_exe   # -> answer()=42
# host-.so fixture (scratchpad): proves the loadLibrary plumbing
acl run ndkdemo.apk --main=com.acl.ndkdemo.NativeMain   # -> "loaded libacldemo; answer()=42"
# a real arm-only app reports the mismatch:
acl run <arm-only>.apk                                  # -> "no host-compatible ABI ... bionic bridge"
```
