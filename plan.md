# Clean-Room Android Compatibility Layer — C++ Reimplementation Plan

**Status:** Draft v1 — 2026-07-09
**Name:** `a4l` (renamed from the `acl` codename on 2026-11-13)
**Goal:** A GPLv3-free, C++23-first runtime that executes unmodified Android APKs on desktop/embedded Linux (Wayland-native), functionally comparable to the GPL-3.0 predecessor, without incorporating or deriving from the predecessor's GPL-3.0-or-later source.
**License target for output:** Apache-2.0 (maximizes AOSP code reuse; MIT dual-license optional).

---

## 0. Objective and non-goals

**Objective.** Run Android applications (activity lifecycle, View UI, SurfaceView/GL, basic media/audio/input) as first-class Wayland clients on glibc Linux, with a C++23 native core and a Java framework layer sourced from AOSP (Apache-2.0), not from the predecessor.

**Non-goals (v1):**
- Google Play Services / SafetyNet / DRM (Widevine) — out of scope; microG (Apache-2.0) is a later opt-in.
- Full CDD compliance or Play certification.
- Android-as-a-container (Waydroid model). This is a translation layer, not virtualization.
- Telephony, NFC, sensors beyond a stub HAL.

---

## 1. Legal and licensing strategy

> Not legal advice. If this is commercial or Toyota-adjacent, route the protocol below through counsel before Phase 0 exits.

### 1.1 What GPLv3 actually reaches

The predecessor's copyright covers its **expression**: its C source, its hand-written Java shims, comments, file structure, and non-trivial organizational choices. It does not cover:

- The **Android API surface** (`android.*` signatures) — authored by Google, published in AOSP under Apache-2.0 and in SDK stubs. *Google v. Oracle* (2021) resolved Google's API copying as fair use rather than declaring APIs uncopyrightable, so the defensible position is not "APIs are free" but "we took the API from its Apache-2.0 upstream, not from the predecessor."
- **Ideas and methods**: "map ANativeWindow to wl_surface," "translate Looper to epoll," "run DEX on host ART." Reimplementing these from independent design is not infringement.
- **Interface facts**: which GTK symbols the predecessor calls, which framework classes it stubs. Facts are not expression, though wholesale reproduction of a curated inventory can edge toward it.

### 1.2 Strategic pivot: AOSP-down, not GPL-across

The single biggest de-risking move: **treat AOSP as the upstream and the predecessor as a competitor whose tree is never opened again.** Nearly everything the predecessor provides has a permissively licensed ancestor:

| Component | Permissive source | License |
|---|---|---|
| Java framework (`android.*`) | AOSP `frameworks/base` | Apache-2.0 |
| Runtime (DEX execution) | AOSP `art` (host build) | Apache-2.0 |
| UI rendering | AOSP `frameworks/base/libs/hwui` (Skia-based) or direct Skia | Apache-2.0 / BSD |
| APK/zip handling | AOSP `system/libziparchive` | Apache-2.0 |
| Resource table (resources.arsc, AAPT2 format) | AOSP `frameworks/base/tools/aapt2`, `libandroidfw` | Apache-2.0 |
| Bionic ABI for NDK libs | AOSP `bionic` | BSD |
| Binder (if used) | mainline kernel `binderfs` + AOSP `libbinder` | GPLv2 (kernel, fine) / Apache-2.0 |
| Conformance oracle | Android CTS | Apache-2.0 |
| DEX tooling | d8/r8 | BSD-3 |
| Media decode | FFmpeg (per prior carlinkit-cxx analysis) / V4L2 M2M | LGPL-2.1 / kernel UAPI |

What remains genuinely "predecessor-shaped" — the Linux desktop glue (windowing, input, audio routing, app launcher) — is exactly the part being redesigned anyway around wcs + Skia + PipeWire + drm-cxx, so there is no temptation to mirror the predecessor's expression there.

### 1.3 Clean-room protocol

Standard two-compartment (Chinese wall) model, adapted for a small team:

1. **Specification compartment.** May read the predecessor, GPLv3 code, anything. Produces *functional specifications only*: behavior, interfaces, protocols, test vectors. No code excerpts, no paraphrased code, no file-structure mirroring. Every spec carries a provenance header listing sources consulted.
2. **Sanitization gate.** Each spec is reviewed (counsel or trained reviewer) to strip expressive content before release to implementers.
3. **Implementation compartment.** Never opens the predecessor's tree. Works only from: sanitized specs, AOSP, Android SDK docs, CTS, and black-box observation of app behavior.
4. **Provenance discipline.** SPDX headers on every file; DCO sign-off on every commit; a `PROVENANCE.md` per subsystem naming permitted references; CI check rejecting commits touching a denylisted path/import.
5. **Contamination log.** Any accidental exposure is logged with scope and mitigation (reassignment of the affected module to an unexposed implementer).

### 1.4 The known contamination problem — handle explicitly

Prior work on the predecessor→Wayland plan involved **detailed, documented reading of the predecessor's source** (GTK dependency inventory G1–G13, per-file symbol counts across 49 files, backend-seam analysis). Under a strict protocol, the person who did that reading is a **spec author, not an implementer**, for any module whose predecessor's counterpart they studied.

Practical options, in descending order of defensibility:

- **A (strongest):** Prior-exposed engineer writes specs and reviews; unexposed engineer(s) implement. Only viable with ≥2 people.
- **B (pragmatic solo):** Quarantine all prior predecessor notes as spec inputs. Rely on (i) the AOSP-down strategy — the implementation's actual ancestry is Apache-2.0 AOSP, provably so via imports and structure; (ii) the fact that prior exposure was interface-inventory level (facts), not expressive internals; (iii) a from-scratch architecture (wcs/Skia/PipeWire/C++23 CRTP) that shares no structural DNA with the predecessor's C/GTK/GObject design. Document this reasoning in-repo. Residual risk is nonzero; counsel should size it.
- **C:** Time-gap + independent redesign ("mental clean room") — weakest, mention only to reject.

Recommendation: **B now, upgrade to A** the moment a second contributor exists. Either way, from today: **do not open the predecessor's tree again.**

### 1.5 License hygiene rules for the new tree

- Permitted: Apache-2.0, BSD, MIT, ISC, Zlib, LGPL-2.1 (dynamic link only, per prior FFmpeg analysis), GPLv2 kernel UAPI headers (syscall boundary, not linking).
- Forbidden in-tree or statically linked: GPLv3, LGPLv3, AGPL. CI license scanner (scancode-toolkit or `reuse lint`) as a gate.
- OpenJDK (GPLv2 + Classpath Exception) acceptable if the OpenJDK runtime path is chosen — but see ADR-1, ART is recommended, mooting this.

---

## 2. Architecture overview (C++23)

```
┌────────────────────────────────────────────────────────────┐
│  APK (unmodified)                                          │
├────────────────────────────────────────────────────────────┤
│  Java framework: AOSP frameworks/base subset (Apache-2.0)  │
│  android.app / android.view / android.content / ...        │
├──────────────── JNI boundary ──────────────────────────────┤
│  ART host runtime (AOSP, Apache-2.0)                       │
├────────────────────────────────────────────────────────────┤
│  a4l native core (C++23, new code, Apache-2.0)          │
│  ├─ runtime/     ART embedding, class-loader plumbing,     │
│  │               DEX/OAT cache, JNI registration tables    │
│  ├─ pkg/         APK parse, resources.arsc, manifest,      │
│  │               split APK/ABI selection (libandroidfw)    │
│  ├─ wm/          Window/Activity surface mgmt →            │
│  │               wcs (wayland-cxx-scanner bindings):       │
│  │               xdg-shell, wl_subsurface (SurfaceView),   │
│  │               wp_fractional_scale_v1 + wp_viewport,     │
│  │               xdg-activation-v1                         │
│  ├─ gfx/         Skia (SHM + EGL + Vulkan-dmabuf canvases  │
│  │               already landed in wcs); hwui evaluation   │
│  ├─ input/       libinput/wl_seat → MotionEvent/KeyEvent;  │
│  │               IME via zwp_text_input_v3                 │
│  ├─ audio/       AAudio/OpenSL shim → PipeWire             │
│  │               (tonic_engine lineage)                    │
│  ├─ media/       MediaCodec shim → V4L2 stateless M2M      │
│  │               primary, FFmpeg sw fallback (LGPL, dyn)   │
│  ├─ svc/         In-process service hub (Binder-less):     │
│  │               ActivityManager/WindowManager/Package-    │
│  │               Manager/Clipboard/etc. — seqlock/SPSC     │
│  │               patterns per ihs_shared where hot         │
│  ├─ ndk/         libandroid.so, ANativeWindow, ALooper,    │
│  │               AAssetManager; bionic-namespace loader    │
│  │               (Phase 4)                                 │
│  └─ shell/       CLI launcher, per-app data dirs,          │
│                  xdg desktop integration, portals          │
└────────────────────────────────────────────────────────────┘
```

Threading model follows the ivi-homescreen reactor discipline: one shared control-plane reactor, per-window render threads, ART owning app main-thread semantics (Looper mapped onto epoll via the reactor).

---

## 3. Key design decisions (ADRs)

**ADR-1 — DEX execution: host ART (AOSP), not OpenJDK+DEX translation.**
Apps ship DEX; ART preserves invoke-custom, hidden-API semantics, `dalvik.system.*`, and JNI behavior exactly. OpenJDK + d8-reversal loses fidelity and drags in `dalvik.*` shims. Cost: ART host build outside the Android build system is the hardest single infrastructure item (see R-1). Decision gate at end of Phase 1: if host ART cannot be stood up in budget, fall back to OpenJDK+CE with a `dalvik.system` compatibility layer and accept the fidelity loss.

**ADR-2 — Java framework: vendored AOSP subset, not hand-written stubs.**
Vendor curated packages from `frameworks/base` (view, graphics, content, os, app, util, text, widget) at a pinned API level (target: 33/Tiramisu — post-scoped-storage, pre-heaviest-Binder-coupling; revisit). Replace the native underside (`android_view_*`, `android_os_*` JNI regs) with `a4l` implementations. Where a class is hopelessly entangled with SystemServer/Binder, substitute a minimal source-compatible shim — written from SDK docs, never from the predecessor.

**ADR-3 — IPC: in-process service hub; binderfs deferred.**
Single-process model (app + framework services in one address space) eliminates Binder for v1. `svc/` exposes the same Java-visible Manager APIs backed by direct C++ calls. binderfs + libbinder is a Phase 5 option if multi-app isolation or real `ServiceManager` semantics become necessary; note binderfs is mainline but frequently not enabled in distro kernels.

**ADR-4 — Rendering: Skia direct via wcs canvases; evaluate hwui in Phase 2.**
wcs already has skia-shm, skia-egl, and skia-vulkan-dmabuf canvases landed. Start with Skia-backed `Canvas`/`Surface` JNI. hwui (AOSP's own Skia-based renderer, incl. RenderThread, damage, HW layers) is the higher-fidelity endgame but has deep `gui/` (BufferQueue/SurfaceFlinger) coupling; Phase 2 spike decides vendored-hwui vs. continued direct-Skia. SurfaceView maps to `wl_subsurface` (decision already made in prior predecessor-replacement work — reusable here as *our* design, independent of the predecessor).

**ADR-5 — NDK native code: staged.**
Phase ≤3: Java/Kotlin-only apps (reject APKs with `lib/` unless `extractNativeLibs`-irrelevant). Phase 4: bionic linker namespaces on glibc host — build AOSP `bionic` + `linker` for host, load app `.so` in a bionic namespace bridged to `a4l`'s libandroid/EGL/vulkan trampolines. This is the same class of problem libhybris solves in reverse; do **not** consult libhybris code without a license check (it's Apache-2.0 — fine).

**ADR-6 — Windowing/scaling.** `wp_fractional_scale_v1` + `wp_viewport` primary, integer `wl_output.scale` fallback — identical policy to the ivi-homescreen physical-pixel patch; implement as shared code if practical.

---

## 4. Phases and gates

### Phase 0 — Legal + skeleton (2–3 wks)
- Clean-room protocol doc, provenance/CI license gates, repo layout, SPDX/DCO enforcement.
- AOSP mirror pinned (repo manifest, api-33 tags); extraction tooling for subsetting `frameworks/base`.
- **Gate G0:** counsel (or documented self-review) signs the §1.4 posture; CI rejects GPLv3 license IDs.

### Phase 1 — Runtime spine (6–10 wks)
- ART host build standing alone: boot classpath from vendored framework, `JNI_CreateJavaVM`-equivalent embedding, DEX→OAT cache dir.
- `pkg/`: APK open (libziparchive), manifest binary-XML parse, resources.arsc via libandroidfw, ABI/split selection.
- Headless milestone: execute an activity-less DEX `main()`, log via framework `Log` → spdlog/DLT facade (reuse ihs_shared three-layer facade design).
- **Gate G1:** `adb`-free `a4l run app.apk` executes Java code, passes a 50-test CTS-derived smoke set (libcore + util). ADR-1 fallback decision here.

### Phase 2 — Window + draw (8–12 wks)
- Activity lifecycle driver (create/start/resume; state machine spec'd from SDK docs + CTS `ActivityLifecycleTests`).
- ViewRootImpl-equivalent native side: Choreographer on `wp_presentation`/frame callbacks, Skia canvas, damage-rect path (apply Flutter-engine damage lessons).
- Input: wl_seat → MotionEvent/KeyEvent pipelines; event-driven dispatch, no fixed-timeout polls (per issue-#261 philosophy).
- **Gate G2:** a non-trivial open-source Java app (e.g., a K-9-class or simple game APK, chosen for no-NDK/no-GMS) renders and is interactive on Mutter, Sway, and agl-compositor.

### Phase 3 — Platform services + audio/media (8–12 wks)
- `svc/`: PackageManager (installed-app db), ActivityManager (task/back stack), Clipboard (wl_data_device via wl::DataDevice), SharedPreferences, ContentResolver (file/media providers), notifications → xdg portals/fdo notifications.
- Audio: AudioTrack/AAudio → PipeWire; media: MediaCodec → V4L2 M2M with FFmpeg fallback; MediaPlayer facade.
- IME via zwp_text_input_v3; xdg-activation for launch/focus.
- **Gate G3:** CTS subset pass-rate targets per module (define in Phase 2 exit); 5-app compatibility list green.

### Phase 4 — NDK/bionic (10–16 wks, parallelizable)
- Host bionic + linker namespaces; libandroid, ANativeWindow→wl_egl_window/dmabuf, ALooper→reactor, AAssetManager.
- EGL/Vulkan trampolines into host Mesa; `AHardwareBuffer` on dmabuf (drm-cxx allocator lineage; explicit-sync/format-modifier discipline).
- **Gate G4:** an NDK game (SDL-based APK) runs; GL and Vulkan smoke CTS subsets pass.
- 
### Phase 5 — Hardening + distribution (ongoing)
- Multi-app process model decision (fork/exec per app; binderfs revisit), sandboxing (bwrap/portals; the boundary it applies to is fixed by ADR-0009), Flatpak packaging (ties into existing Flathub catalog/launcher specs), per-board bring-up (RK3588, SA8155P) via emb_cli artifact store.

---

## 5. Conformance and test strategy

- **CTS as oracle** (Apache-2.0): curate module subsets per phase; run in-tree via a `cts-runner` harness. Pass-rate is the compatibility metric, not "looks right."
- Tiered like wcs: **Tier A** unit (C++ Catch2/gtest + Java junit via ART), **Tier B** headless integration (wl socket-pair harness — reuse the wcs Tier C live-dispatch gate design), **Tier C** compositor matrix (Mutter/Sway/wlroots/agl) in CI via wlheadless.
- **App corpus:** 10–15 F-Droid apps (redistributable) pinned by hash, launched under a screenshot+input-replay harness; per-app status page.
- Golden-image rendering diffs for View/Canvas against CTS reference where available.

## 6. Risk register

| ID | Risk | L | I | Mitigation |
|---|---|---|---|---|
| R-1 | ART host build outside soong is a tarpit (blueprint deps, apex, prebuilts) | H | H | Timebox 4 wks; pin to an api-33 tag with known host-test targets; ADR-1 OpenJDK fallback ready |
| R-2 | frameworks/base subset extraction pulls the world (Binder, SystemServer) | H | H | Shim-at-Manager-boundary rule; measure vendored class count per package; hard budget |
| R-3 | Clean-room defensibility given prior the predecessor exposure (§1.4) | M | H | Posture B documented; no further the predecessor access; counsel review; second engineer → posture A |
| R-4 | Real apps use hidden APIs / reflection into internals | H | M | Vendored AOSP (not stubs) covers most internals; hidden-API allowlist like ART's own |
| R-5 | hwui vs direct-Skia wrong call → repaint perf or fidelity gap | M | M | Phase-2 spike with per-stage budgets (reuse LVGL benchmark methodology) |
| R-6 | bionic-on-glibc namespace bridging (TLS, ifuncs, pthread ABI) | M | H | **De-risked (2026-07-16):** in-process bionic linker bootstrap PROVEN on the glibc host (`tools/ndk-loader-spike/`, `nativeAnswer()=42`); TLS handled by `%fs` save/restore + permanent-TCB migration. Remaining is integration, not feasibility. Java-only remains shippable |
| R-7 | GMS-dependent apps dominate user expectations | H | M | Scope statement; microG later; publish compat list early |
| R-8 | GPLv3 creep via transitive deps (e.g., readline, some codecs) | L | H | CI license scanner gate; dynamic-link-only LGPL policy |
| R-9 | Wayland IME/clipboard/activation gaps on some compositors | M | M | Same portability matrix + fallbacks as the flatpak-activation plugin spec |
| R-10 | Untrusted app native code runs unrestricted in a4l's single glibc process (no per-app UID/SELinux/seccomp; a malicious/buggy `.so` reaches ART, Mesa, the Wayland conn) | M | H | Integrity = one-time **APK v2/v3 signature verification** in `pkg/` (the `.so` are covered by the whole-APK sig; not a per-load or loader concern) -- implemented 2026-11-14 in `pkg/apk_verify.cpp`, enforced on request (`--verify=require`, `--signer`). Containment = **ADR-0009**: a process boundary at the dma-buf seam (`Backend::ArtSplit`), with seccomp, namespaces/Landlock on the child; a seccomp filter on the shared process is rejected as containment (a filter binds a thread, and app and a4l code share every thread, fd and page). Dev corpus is user-vetted; untrusted APKs are gated on ADR-0009 C1-C5, of which only C1 is implemented |

## 7. Effort estimate

Assumes one senior engineer full-time with existing wcs/drm-cxx/PipeWire assets; ranges widen ±30% at R-1/R-2.

| Milestone | Estimate |
|---|---|
| G0 legal/skeleton | 2–3 wks |
| G1 runtime spine | 6–10 wks |
| G2 windowed Java app | 8–12 wks |
| G3 services/audio/media | 8–12 wks |
| **Cumulative to "useful" (G3)** | **~6–9 months** |
| G4 NDK/bionic | +10–16 wks |
| Broad compatibility | multi-year, community-scaled |

## 8. Repo layout and provenance

```
a4l/
├── LICENSE            Apache-2.0
├── PROVENANCE.md      global source policy (§1.3)
├── third_party/
│   ├── aosp/          vendored, pinned, unmodified-where-possible (Apache-2.0)
│   ├── skia/          BSD
│   └── ...            each with LICENSE + METADATA (origin URL, tag, SHA)
├── specs/             sanitized functional specs (spec-compartment output)
├── src/{runtime,pkg,wm,gfx,input,audio,media,svc,ndk,shell}/
├── tests/{unit,integration,cts-runner,app-corpus}/
└── docs/adr/          ADR-0001..N
```

Every `src/` file: SPDX header + `// Provenance: specs/xxx.md, AOSP docs, CTS` comment. Every commit: DCO. CI: `reuse lint`, license scan, denylist path check.

---

## Appendix A — Permitted / forbidden reference matrix

| Reference | Status |
|---|---|
| AOSP source (Apache/BSD dirs) | ✅ read, vendor, modify |
| Android SDK docs, developer.android.com | ✅ |
| CTS source and results | ✅ |
| Black-box behavior of apps / real devices | ✅ |
| libhybris, Anbox (Apache-2.0) | ✅ read with license check per file |
| Waydroid (GPL-3.0 components) | ❌ code; ✅ published docs/blog posts (record in provenance) |
| **predecessor's tree, predecessor MRs/diffs** | ❌ from this date forward |
| Prior prior analysis notes (G1–G13 etc.) | ⚠️ quarantined spec inputs only; never pasted into src/ |

---

# Addendum A01 — Automotive Profile (default ON)

**Status:** Draft v1 — 2026-07-09. Amends §2–§7. Base plan text unchanged; conflicts resolve in favor of this addendum.

## A01.1 Option semantics

Build option `ACL_PROFILE_AUTOMOTIVE` (CMake), **default ON**. Gates three components: `svc/car/` (vehicle services), `hosts/cal/` (Car App Library template host), `hosts/media/` (media-app bridge). Runtime enablement via the config snapshot API (ihs_shared-style), so a single binary can ship with the profile compiled in and disabled per-target. `OFF` builds must produce zero automotive symbols (verified by a CI nm-diff gate) — the phone-profile path stays a clean subset.

Licensing delta: none. `packages/services/Car`, `car-lib`, AIDL VHAL definitions, `androidx.car.app`, and automotive CTS modules are Apache-2.0. GAS remains out of scope (R-7 unchanged).

AOSP pinning delta: `packages/services/Car` and automotive CTS pin to an **automotive release tag** (Car API is versioned independently of platform API); document the platform-tag ↔ car-tag pairing in `third_party/aosp/METADATA`.

## A01.2 New components

### `hosts/media/` — media-app bridge (fast path, pre-Phase-2)
`MediaBrowserService` / `MediaSession` apps render no UI by design — the OEM shell does. Bridge them headlessly:
- Vendored `media` framework subset + session stack; support both legacy support-lib and media3 session interfaces (compat shim, see RA-2).
- Northbound: an `ihs_shared`-published surface (browse tree snapshots + playback state via seqlock, commands via SPSC ring) consumed by an ivi-homescreen Flutter media UI plugin; plus a **MediaSession→MPRIS bridge** so stock Linux desktops get controls for free.
- Depends only on G1 (runtime spine) + PipeWire audio from Phase 3's audio slice — **pull AudioTrack/AAudio→PipeWire forward** into this workstream.

### `hosts/cal/` — Car App Library template host
`androidx.car.app` apps never draw; they serialize a template tree (`Bundleable` graph) to a platform-provided host, which renders and enforces distraction rules. We implement the host:
- In the ADR-3 single-process model the app↔host AIDL boundary collapses to direct calls behind the same interfaces (CarAppBinder-equivalent), so no Binder is needed; keep the serialization layer intact anyway for version skew tolerance across CAL releases.
- **Rendering decision (ADR-7):** render templates in **Flutter via ivi-homescreen** (host UI is ours, not Android Views — no hwui/ViewRoot dependency), with a native-Skia fallback renderer for a4l-standalone use. This makes the CAL host shippable **before Phase 2 completes**: templates + media constitute a usable IVI app story with zero Android View stack.
- Template set staged: Tier 1 (List, Grid, Pane, Message, LongMessage), Tier 2 (Tab, SearchTemplate, SignIn), Tier 3 (NavigationTemplate + surface embedding — needs SurfaceView/wl_subsurface path from Phase 2/4).

### `svc/car/` — vehicle services
- Vendored `android.car` (car-lib) with `CarPropertyManager` backed by a C++ **vehicle property hub**: the VHAL seam re-cut for single-process — typed property table, seqlock snapshot publication, SPSC subscription rings, change/continuous rate semantics per VHAL property config; SIGUSR2 JSON dump surface for debug.
- Southbound adapters as plugins: `mock` (replayable JSON traces, default for CI), `can` (SocketCAN), `someip` (vsomeip, dyn-linked — license check: vsomeip is MPL-2.0, permitted), later `vhal-aidl` passthrough if binderfs is ever adopted (Phase 5).
- `CarUxRestrictions`: real service, **explicit configuration** — default `unrestricted` with a loud log line; wire to speed/parking-brake property when the hub carries it. Never silently absent (template apps query it).
- Non-goals restated: headless system user model, occupant zones, multi-display-per-user (revisit only if output-enumeration work ever demands per-occupant displays).

## A01.3 Gate changes

| Gate | Change |
|---|---|
| **G1.5a (new)** | Media bridge: an F-Droid `MediaBrowserService` app browses + plays through PipeWire, controlled from MPRIS and from a Flutter test UI. Requires G1 + audio slice only. |
| **G1.5b (new)** | CAL host Tier-1 templates: a `androidx.car.app` sample (AOSP `car-samples`, Apache-2.0) renders via Flutter host and is interactive. |
| G2 | Unchanged (phone/View path), no longer on the automotive critical path. |
| G3 | Adds: property hub passes a curated automotive-CTS/VHAL-semantics subset against the `mock` adapter; CarUxRestrictions service present. |
| G4 | Adds: NavigationTemplate surface path (Tier-3 templates) once wl_subsurface/AHardwareBuffer lands. |

Net effect: **first useful automotive milestone moves from ~G3 (6–9 mo) to G1.5 (≈3–4 mo)**, because media + Tier-1/2 templates bypass the Android View stack entirely.

## A01.4 Risk register additions

| ID | Risk | L | I | Mitigation |
|---|---|---|---|---|
| RA-1 | CAL `Bundleable` protocol skew across androidx.car.app versions breaks host↔app compat | M | M | Keep serialization boundary even in-process; pin supported CAL version range; CTS-for-CAL host tests |
| RA-2 | Legacy support-lib vs media3 session duality doubles media bridge surface | M | M | media3 first (compat layer covers legacy for most apps); corpus-driven |
| RA-3 | UxRestrictions stub in a moving vehicle is a safety/regulatory exposure (driver distraction) | L | H | Explicit config + loud default; document as integrator responsibility; wire to real signals before any vehicle deployment |
| RA-4 | Property hub semantics drift from VHAL spec (rate limiting, staleness, zoned props) | M | M | Treat AIDL VHAL docs + VTS tests (Apache-2.0) as the spec; mock-adapter replay tests in CI |
| RA-5 | Flutter-host dependency couples a4l releases to ivi-homescreen ABI | M | L | CAL host talks only via ihs_shared stable ABI; native-Skia fallback keeps standalone builds honest |

## A01.5 Effort delta

| Item | Estimate |
|---|---|
| Media bridge (G1.5a), incl. pulled-forward PipeWire audio | 3–5 wks |
| CAL host Tier 1–2 + Flutter renderer (G1.5b) | 5–8 wks |
| Vehicle property hub + mock/can adapters + car-lib vendoring | 5–8 wks |
| Tier-3 NavigationTemplate (post-G4 surface path) | 2–4 wks |
| **Automotive profile total (largely parallel to phone path)** | **~3–5 months, overlapping** |

## A01.6 Provenance additions

Permitted: `packages/services/Car`, `androidx.car.app` (Jetpack repo), AOSP `car-samples`, AIDL VHAL definitions + VTS, automotive CTS — all Apache-2.0; vsomeip (MPL-2.0, dynamic). Forbidden: GAS binaries/docs-under-NDA, any OEM proprietary VHAL implementations.

---

# Addendum A02 — Rendering/Windowing Constraint: wcs + Skia Committed

**Status:** Draft v1 — 2026-07-09. Binding architectural constraint; amends ADR-4, ADR-7, Phase 2, A01. Conflicts resolve in favor of this addendum.

## A02.1 Constraint

All native windowing in a4l goes through **wayland-cxx-scanner (wcs)** bindings; all rasterization goes through **Skia**. No GTK, no Qt, and no hard Flutter dependency in the a4l core. Concretely:

- Surfaces: xdg-shell toplevels/popups, `wl_subsurface` (SurfaceView), `wp_viewport` + `wp_fractional_scale_v1`, `xdg-activation-v1`, `wp_presentation` — all via wcs CRTP bindings. Seat/cursor/data-device via the landed `SeatManager`, `wl::CursorManager`, `wl::DataDevice`.
- Pixels: wcs Skia canvases as the only render backends — `skia-shm-canvas` (guaranteed path), `skia-egl-canvas`, `skia-vulkan-dmabuf` (preferred on RK3588/SA8155P/RADV). Backend selection at runtime, same probe order discipline as ivi-homescreen.
- Known wcs gaps that now sit on this project's critical path: **dmabuf feedback v4** and **syncobj** bindings (open items from the tip-of-tree scan). Schedule them as wcs upstream work in Phase 2, since the vulkan-dmabuf canvas on multi-GPU/scanout targets wants both.

## A02.2 ADR-4 (revised) — Skia-direct committed; hwui dropped

The Phase-2 hwui-vs-Skia spike is **canceled**. hwui is not vendored; no BufferQueue/RenderThread/SurfaceFlinger machinery enters the tree. Rationale beyond the constraint itself: AOSP's `android.graphics` JNI is already a thin Skia wrapper, so the vendored framework's Canvas/Paint/Bitmap/Path/Typeface bindings re-target host Skia with a shim rather than a rewrite — hwui's value was HW-layer/damage orchestration, which we own natively instead.

What we re-own natively (spec sources: SDK docs + CTS + Flutter-engine damage lessons, per base plan):
- **RenderThread-equivalent:** one render thread per toplevel (ivi-homescreen reactor discipline), Choreographer driven by frame callbacks + `wp_presentation` feedback.
- **Damage:** buffer-age-aware partial repaint on the EGL/SHM paths; full-frame on vulkan-dmabuf until syncobj/feedback land.
- **Text:** Skia + HarfBuzz/ICU directly; `android.text`/`Typeface` JNI mapped to SkTypeface/SkFont with fontconfig-backed font resolution.
- Hardware `View` layers → SkSurface-backed layer cache; `RenderEffect`/shaders → SkRuntimeEffect (AGSL and SkSL are near-identical — flag divergences per CTS).

**New pinning rule:** Skia has no stable API; pin the Skia milestone to the branch used by the vendored `frameworks/base` tag (api-33 ⇒ Skia m-branch of that release) *or* carry a small `gfx/skia_compat.h` shim if wcs canvases want a newer milestone. Record the pairing in `third_party/*/METADATA`. wcs and a4l must agree on one Skia build (single copy, no ODR games).

## A02.3 ADR-7 (revised) — CAL host renders native Skia; Flutter optional

Inverts A01's ordering: the **native Skia template renderer is primary**; the ivi-homescreen/Flutter renderer becomes an optional integration behind the ihs_shared ABI (unchanged interface, so nothing in A01's data-plane design moves). Consequences:
- a4l-standalone is fully functional on any wlroots/Mutter/agl compositor with zero Flutter runtime.
- RA-5 (Flutter ABI coupling) downgraded to L/L.
- Template renderer becomes the first consumer of the Skia UI substrate (Tier-1 templates are lists/panes/text — a good forcing function for the text stack before Android View work needs it). G1.5b now requires the Skia text path, so HarfBuzz/ICU/fontconfig bring-up moves ahead of G1.5b instead of Phase 2.

## A02.4 Gate and risk deltas

| Item | Change |
|---|---|
| G1.5b | Renders via native Skia host (Flutter variant optional, non-gating) |
| G2 | Spike removed; adds: damage-path correctness on skia-egl (buffer age) + pixel-diff harness vs CTS references on skia-shm |
| Phase 2 scope | + wcs upstream: dmabuf-feedback-v4, syncobj bindings |
| RA-5 | Downgraded L/L (native renderer primary) |
| **RA-6 (new)** | Skia milestone skew between vendored framework JNI expectations and wcs canvases — M/M. Mitigation: single pinned Skia, `skia_compat.h` shim, pairing recorded in METADATA |
| **RA-7 (new)** | Text-stack fidelity (line breaking, hyphenation, emoji, CJK) lands earlier and without minikin — M/M. Mitigation: vendor AOSP `minikin` (Apache-2.0) over HarfBuzz if SkParagraph/direct shaping misses CTS text metrics; decide at G1.5b exit |

RA-7 note: `minikin` is the honest fallback — it is what `android.text` actually sits on in AOSP, it is Apache-2.0, and it pulls in no hwui.
