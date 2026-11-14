<!-- SPDX-License-Identifier: Apache-2.0 -->
# a4l

Android apps on Linux, without an emulator and without an Android device image.

If a Linux product needs an Android app on it -- an IVI head unit, a kiosk, a panel, a
workstation -- there have been two ways to get one. Run a whole Android OS beside your own,
as an emulator or as a container carrying Android's userspace, its binder, its compositor
and its graphics stack. Or get the app ported, which is usually not on offer, because you do
not own the app. The first duplicates the stack the product already ships and taxes every
frame that crosses between the two.

a4l takes a third position: run the app, not the OS. The APK's DEX executes on real ART, in one
glibc process on x86-64, alongside a4l's native core and the app's own bionic-ABI `.so` files. The
Java framework is a vendored Apache-2.0 AOSP subset. There is no second compositor, no binder, no
device image and no VM boundary -- the app is an `xdg-toplevel` on the compositor the product
already runs. On arm64 the only stock host ART is bionic, which a glibc process cannot `dlopen`, so
that path runs ART in its own all-bionic process and keeps the platform in the first
([`docs/split-process-art.md`](docs/split-process-art.md)).

a4l is clean-room and **C++23-first**, comparable in function to the GPL-3.0 project that
preceded it while sharing **none** of its source. Output license is **Apache-2.0**. The
automotive profile (media apps + Car App Library templates) is a first-class target and
reaches a useful milestone ahead of the phone/View path.

> **Status: unmodified APKs render — Android UI toolkits, Flutter, WebView and video.** Of 18
> F-Droid apps run headless on `a4l run --vm=art`, 15 reach layout and 3 fail; reaching layout means
> the app booted without an `onCreate` crash and its decor measured and laid out, which is not a
> pixel check. Rendering is verified on screen for Jetpack Compose (`SVS.pdfinspector`),
> Views/Material (`opendrummer`, `a2dp.Vol`, `batterytemperature`) and a custom-View clock
> (`octalsuntime`). A real app needs `ACL_PRESENT_GPU=1`, the zero-copy path that draws through Skia
> (Ganesh Vulkan) into a dma-buf with a tiled format modifier; the default shm path can only present
> a4l's own Bitmap shim. CI enforces the clean-room and license discipline.
>
> **Flutter APKs built with the Flutter tooling render on screen too**, on an unmodified engine:
> `libflutter.so` loads against a4l's own bionic-side `libandroid.so`, its EGL/GLES forward to host
> Mesa, and for a native-GL app Mesa renders and commits a4l's toplevel `wl_surface` itself. Text,
> network images, `Texture`-widget external textures (an a4l dma-buf imported as an `EGLImage`
> through `ImageReader`) and a `video_player` clip that decodes, plays and loops have each been seen
> on screen.
>
> Also landed, each one named as a wall in the 2026-07-18 corpus notes: Chromium **WebView**
> composites through its `AwDrawFn` Vulkan functor (a full OSM map, an EPUB reader); a video
> **`SurfaceView`** hole-punches onto a `wl_subsurface` below the toplevel, Android's own model; and
> the legacy **`gles_jni`** EGL10/GLES JNI is registered, so `GLSurfaceView` apps (libGDX) run and
> take input. What is still open is narrower than those: apps whose own GL draws to an FBO can
> present nothing, a GLES3-heavy app can link its whole native stack and then trip a robustness bug
> in a4l's JNI bridge, and a4l does not confine an app's native code. APK signatures are checked,
> and required only when asked with `--verify=require` or a pinned `--signer` (see
> [`SECURITY.md`](SECURITY.md)). `docs/app-corpus-status.md` tracks the corpus app by app and dates
> every claim. The authoritative design is [`plan.md`](plan.md); contributor guidance is in
> [`CONTRIBUTING.md`](CONTRIBUTING.md).

## One process, and three paths that never copy a frame

![a4l layer stack: APK, ART, AOSP framework and a4l core in one glibc process](docs/a4l-stack.svg)

Figure 1 draws the boundary the rest of the design follows. Everything inside the dashed
enclosure is one glibc process on x86-64; everything below it is the stack the product already
ships, unchanged. No Android compositor, no binder and no second graphics driver appear in the
drawing because none of them is in the process. arm64 is the stated exception, noted on the
sheet. The accent marks what sits on a zero-copy path.

![a4l present paths: GPU, native GL, video, and the wl_shm fallback](docs/a4l-present-paths.svg)

Figure 2 follows one frame from the app to the display controller on each of the four present
paths. The first three reach the compositor with no CPU copy, and on the native-GL path a4l
allocates no buffer at all -- Mesa renders and commits a4l's own `wl_surface` inside
`eglSwapBuffers`. The fourth is the default, and it is the usual reason a first run without
`ACL_PRESENT_GPU=1` looks empty.

## Read these first

- [`plan.md`](plan.md) — full design, ADRs, phases, risk register (source of truth).
- [`PROVENANCE.md`](PROVENANCE.md) — **enforced** source/license policy. The overriding rule:
  never open the GPL-3.0 predecessor's tree; take the Android API surface from Apache-2.0 AOSP
  (AOSP-down, not GPL-across).
- [`docs/clean-room.md`](docs/clean-room.md) — the clean-room protocol and current posture.
- [`docs/adr/`](docs/adr/) — architecture decision records.
- [`CONTRIBUTING.md`](CONTRIBUTING.md) — SPDX + provenance + DCO requirements.

## Build

Build system is **Meson** (native to the Wayland/PipeWire/Mesa stack this integrates against).
Heavy vendored deps keep their own builds — **Skia via GN, AOSP ART/frameworks via Soong** — and
are consumed as prebuilt imported artifacts; the Phase 0 skeleton needs none of them.

```sh
meson setup build
meson compile -C build
./build/src/shell/a4l           # prints version + linked module roster

meson test -C build                # Tier A unit tests (needs: meson wrap install catch2)
```

Automotive profile is **ON by default**. Build the phone subset (must emit zero automotive
symbols) or cross-compile for a board:

```sh
meson setup build-phone -Dprofile_automotive=false
meson setup build-rk3588 --cross-file cross/rk3588.ini
```

## Layout

```
src/{runtime,pkg,wm,gfx,input,audio,media,svc,ndk,shell}   native core (C++23)
src/svc/car/                                                vehicle property hub (automotive)
hosts/{cal,media}                                           Car App Library + media bridges (automotive)
third_party/{aosp,skia}                                     vendored, pinned, prebuilt-consumed
specs/                                                      sanitized functional specs (clean-room)
tests/{unit,integration,cts-runner,app-corpus}             Tier A/B + CTS oracle + real-app corpus
docs/adr/                                                   architecture decision records
scripts/, .github/workflows/                                CI license/provenance/SPDX gates (Gate G0)
```

## License

Apache-2.0. Copyright 2026 Contributors to a4l, stated in [`NOTICE`](NOTICE).

[`LICENSE`](LICENSE) holds the license text verbatim, because automated license
detection matches that file by content rather than by what it claims; [`LICENSES/`](LICENSES/)
holds the same text under its SPDX identifier, where the [REUSE](https://reuse.software)
specification looks for it. Every file in the tree declares its own license with an SPDX
tag, `REUSE.toml` records the copyright holder for files that cannot carry a header, and
`reuse lint` verifies that nothing is unaccounted for.
