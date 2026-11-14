<!-- SPDX-License-Identifier: Apache-2.0 -->
# ADR-0009: Containment -- a process boundary at the dma-buf seam; seccomp alone rejected

- **Status:** Accepted (boundary decided; no containment is implemented). Refines ADR-0003.
- **Date:** 2026-11-13
- **Provenance:** plan.md §4 Phase 5, §6 R-10; docs/ndk-scope.md (trust model);
  docs/split-process-art.md; ADR-0001, ADR-0003, ADR-0005; `src/ipc/`, `src/runtime/split_vm.cpp`
  as they stand at this date. Kernel interfaces as documented in seccomp(2), landlock(7) and
  user_namespaces(7). No GPL-3.0 sources.

## Context
R-10: an app's native code is arbitrary code in a4l's single glibc process. It reaches ART, Mesa
and the Wayland connection, with no per-app UID, SELinux domain or seccomp filter. R-10 named the
mitigation as "a seccomp filter around the bionic-namespace threads" and deferred it to this ADR.

That mitigation does not hold, for four reasons that are properties of the mechanism, not of a4l:

- **A seccomp filter attaches to a thread, not to a library.** There is no thread that runs only
  app code. A `RegisterNatives`'d app native is invoked on an ART thread through the `%fs`
  trampoline (docs/ndk-scope.md, rung 4b onward), and the same thread runs a4l's own JNI natives --
  the ones that issue DRM ioctls and write the Wayland socket. A filter tight enough to constrain
  the app denies a4l; a filter loose enough for a4l permits the app.
- **File descriptors are per-process.** The Wayland socket, the DRM fds and the PipeWire
  connection are open in the app's fd table. Any filter that lets a4l `sendmsg` on them lets the
  app do the same.
- **Memory is shared.** App code can call a4l and ART functions directly and overwrite their
  data. seccomp narrows syscalls; it says nothing about which code issued one. Filtering on the
  syscall's instruction pointer does not rescue this: the app can jump to a `syscall` instruction
  inside an allowed range.
- **A filter cannot be loosened once installed**, so it cannot be scoped to the duration of an
  app call on a shared thread.

So a filter on the shared process is syscall narrowing, not confinement. Confinement needs an
address space, an fd table and a credential that the app does not share with the platform side.

A second fact, from an unrelated decision: that boundary is already drawn. On arm64 a glibc
process cannot `dlopen` the bionic `libart.so`, so `Backend::ArtSplit` runs ART, the framework and
the render half in `acl-art-host` and leaves present, input, audio and `pkg/` in the a4l process
(docs/split-process-art.md). What crosses is 12 fixed message tags over one `SOCK_SEQPACKET` pair,
with dma-buf, fence and shm fds as `SCM_RIGHTS` (`src/ipc/ipc.h`). It was built for an ABI reason.
It is also where Android puts the same line: the app renders in its own process with the GPU
driver loaded, and the compositor is elsewhere.

State of that path, from `git log` rather than from the doc: S1 (ART boots in the subprocess,
x86-64 and a Pi 4) and the S2 present-sink seam landed 2026-07-15/16; nothing since. First pixels
across the boundary, input, audio and a real APK (S2-S5) are not done.

## Decision
1. **The containment boundary is a process boundary at the dma-buf seam.** Everything an APK
   supplies -- DEX, native libraries, assets -- executes in a child that holds no display, audio
   or session connection. The a4l process is the broker: it owns the Wayland connection or DRM
   master, PipeWire, the portals, and `pkg/`.
2. **seccomp is applied to that child, and is not claimed as containment anywhere else.** The
   child is confined before it executes any byte from the APK: `no_new_privs`, a seccomp-bpf
   allowlist, and either namespaces (user, mount, pid, net, ipc) or Landlock for the filesystem.
   It inherits the IPC socket and a GPU render node and nothing else.
3. **The broker treats every message as hostile.** Each field is validated before it reaches a
   compositor or KMS import.
4. **Integrity stays where R-10 put it, and runs in the broker.** One APK v2/v3 verification in
   `pkg/` when the APK is opened, before the child is spawned.
5. **`Backend::ArtSplit` is the containment seam and is not retired by B2.** A single-libc arm64
   ART would remove the ABI reason for the split; it does not remove this one. The split becomes
   a supported configuration on x86-64 as well.
6. **Single-process remains, for vetted APKs only.** It is the development path and the right
   configuration where the integrator owns or audits the app. It is not a supported way to run an
   untrusted APK, and no hardening of it changes that.

## Consequences
**Gate before "runs untrusted APKs" is claimed.** All five, on the architecture claimed:

| | Condition | Today |
|---|---|---|
| C1 | APK v2/v3 verification in `pkg/`, in the broker | implemented 2026-11-14 (`pkg/apk_verify.cpp`), enforced by `--verify=require` or `--signer`; the runtime reopens the APK by path, so the verified read is not the only read |
| C2 | Split path at S5: a real APK renders and takes input | S1 done; S2 seam wired, no pixels |
| C3 | Native GL and video cross the boundary as dma-buf | not designed; see below |
| C4 | Child confined per Decision 2 | none; `posix_spawn` passes `environ`, no attributes |
| C5 | Broker validates every message; receive path fuzzed | `PresentFrame` fields reach the sink as received |

**What it costs.**
- **The native-GL path changes shape.** Single-process, Mesa commits a4l's own `wl_surface`
  inside `eglSwapBuffers` and a4l allocates no buffer. A confined child has no `wl_surface`. It
  must render to an exported dma-buf and send `PresentFrame`. Still no CPU copy; the "allocates no
  buffer" property is a single-process property. This is the largest piece of C3.
- **One datagram and one fd in each direction per frame.** Not measured. It gets a number at S2.
- **The GPU driver stays reachable from the child.** Render-node ioctls are a large kernel
  surface, and the filter cannot remove them without removing rendering. Android accepts the same
  exposure. Video decode stays in the child for the same reason it renders there: it parses data
  the app supplies.
- **On x86-64 the child is today's whole process minus present, input and audio**: glibc host ART
  plus the bionic bridge of ADR-0005. On arm64 it is all-bionic and needs no `%fs` bridge.

**What it does not fix.** A second process under the same UID is not a boundary by itself:
without C4 the child can open the user's files and, where Yama permits, reach the broker through
`ptrace` or `/proc/<pid>/mem`. Kernel and GPU-driver bugs, side channels and resource exhaustion
are outside what this decides; a memory and CPU bound on the child is follow-on work.

**Follow-on.**
- The seccomp allowlist is recorded from the app corpus (docs/app-corpus-status.md), not written
  by hand, then reviewed. A hand-written list is wrong in both directions.
- The multi-app decision in Phase 5 is constrained by Decision 1: however apps map to processes,
  no app code holds the display connection. Per-app UIDs and binderfs (ADR-0003) remain open and
  are not needed for one confined app.
- ADR-0003 stands for services: the hub stays in-process with the framework, in the child. What
  moves to the broker is the device edge the split already moved.
