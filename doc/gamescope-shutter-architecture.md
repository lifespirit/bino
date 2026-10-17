# Standalone Gamescope shutter-stereo: architecture and first milestone

Status: **architecture selected; no Gamescope stereo support has been implemented**.
Do not connect the 3D Vision emitter to a test that has not achieved reproducible
correct physical stereo-output timing without glasses.

## Decision: Game-capable standalone DRM compositor

Prefer extending [ValveSoftware/gamescope](https://github.com/ValveSoftware/gamescope)
in its `--backend drm` (standalone) mode, rather than using a Bino
`QOpenGLWindow` on KWin or building an entirely new DRM media player.

An arbitrary game **does not generate stereo image pairs** automatically.
Gamescope will accept explicitly marked SBS/top-bottom/dual-eye client
frames from a 3D movie player or stereo-capable game. Games generating
only a monoscopic image need a separate injector/game modification or a
VR/OpenXR conversion path. In particular, a normal Vulkan swapchain
is a mono image, not a 3D Vision frame source.

DRM/KMS control is **necessary but not sufficient**. A page-flip event
signals that the scanout started/chosen buffer switched; it is received
too late to guarantee triggering the matching IR pulse *in advance*.
We must (1) choose the source stereo pair and eye for a targeted
vblank, (2) validate actual atomic-commit/page-flip completion,
(3) synchronize USB early enough against a stable monotonic clock
and keep validating, (4) explicitly recover on dropped/late flips,
(5) validate optically, including the monitor's scanout and LCD
response time and the right/left shutter waveforms.

## libnvstusb capabilities -- verified from our source

Relevant files: `lifespirit/libnvstusb/include/nvstusb.h` and
`src/nvstusb.c`.

* `nvstusb_set_rate(ctx, rate)` programs firmware timing registers.
  The C source calculates period as `1e6/rate`, and has a constant
  `activeTime=2080` microseconds stored in a timer register.
  The code comments interpret this as the time each eye is on,
  **but this is not an optical measurement of shutter openness**.
* `nvstusb_send_eye(ctx, left|right)` sends an 8-byte `0xAA` USB
  command with eye selector `0xFF/0xFE` and firmware timer data.
* The public API provides **no per-call target clock time**,
  **no per-call active duration**, **no measured optical phase**,
  **no proof of an individual shutter transition**, and
  **no confirmed all-shutters-closed / fail-safe command**.
* In principle `activeTime` could be made a configurable parameter
  and the firmware command format modified. That is different from
  a guaranteed, vblank-synchronous physical shutter-control API;
  it must be checked experimentally, without viewing through glasses.
* The existing `nvstusb_swap` offers historical GLX/X11 synchronization
  cases, but `nvstusb_send_eye` does not invoke those and the
  Gamescope DRM session is not driven by GLX.

Therefore shutter *phase and optical state are not guaranteed* by
the current library. Do not claim otherwise.

## Proposed Gamescope implementation boundaries

1. **`src/Backends/DRMBackend.cpp`**:
   attach a monotonically increasing physical flip ID, chosen stereo
   eye, framebuffer identity and target vblank (where supported)
   to each pending atomic commit context, verify the event from
   `page_flip_handler()` and report mismatch/late flips. Do not
   send USB from a page-flip handler: that runs *after* flip.
2. **`src/vblankmanager.cpp`, `src/vblankmanager.hpp`**:
   use Gamescope's existing physical vblank timestamps and deadline
   estimator; give the shutter scheduler a shared reference clock.
   Avoid permanently hard-coded lead times.
3. **`src/steamcompmgr.cpp` + compositor renderer**:
   add an explicit stereo mode accepting paired L/R content; force
   rendering at every physical output slot, even if source updates
   occur only at 24, 30 or 60 Hz. Select eye from physical slot,
   not number of decoded/game-submitted frames.
4. **Separated emitter controller / bounded thread**:
   integrate libnvstusb outside of the DRM event-processing thread.
   Benchmarked USB latency and eye-command timing must be measured
   against the DRM clock. An independent safety state machine must
   suppress 3D on failure (if the emitter supports a confirmed
   shut-off mechanism). Without that, abort and do not promise
   fail-safe physical shutters.
5. **Input formats**:
   first accept full/half SBS with explicit half-width scaling;
   then top-bottom and paired textures; then games or OpenXR
   adapters which can provide two viewpoints. Preserve mono mode.
6. **Verification**:
   independently verify display brightness and physical shutter
   transmission (left/right) with photodiodes/high-speed capture
   before an active-glasses viewing test. No perceived comfort
   assessment by prolonged staring at unstable shutters.

## Stage 0 -- first practical test (unmodified upstream Gamescope)

Use `bash tools/gamescope-drm-baseline.sh` from a **spare local TTY**
with the corresponding seat active, after installing `gamescope`
and `vulkan-tools`. The script will reject an existing graphical
terminal. It launches only `vkcube` in standalone DRM, does not
use libnvstusb and logs output to
`$HOME/gamescope-drm-baseline.log`.

**Do not stop Plasma or kill system services**. Switching back to
its existing VT should restore the desktop; seat-manager behavior
depends on your host configuration. If the DRM backend cannot gain
the seat or detects the wrong AMD GPU/output, resolve that first.

Gamescope flags: `-W/-H` select preferred **output** dimensions,
`-w/-h` select client dimensions, `-r` is **nested game refresh**
(not a guarantee of the physical monitor mode). The DRM backend must
confirm DP-1 is selected at **2560x1440@120** with VRR inactive.
Inspect its reported selected mode rather than trusting the CLI.

Success here means only that the *unmodified* standalone DRM backend
can acquire DP-1 and render a client. This test does **not** prove
120 Hz alternating eye presentation or shutter stability.

## Stage 1 -- first code patch in a Gamescope fork

Use a separate dedicated fork of upstream Gamescope (not a patch to
Bino or an unreviewed upstream commit). Instrument the physical
`page_flip_handler()`, request issuance and GameScope's vblank
estimator, logging target, actual sequence, actual times and
presented framebuffer ID. Do not open USB. Then add a compositor-
generated two-color test pattern whose L/R IDs are checked against
actual flip events at 120 Hz for sustained sessions.

The standalone fork must be built and tested against the user's
Arch Gamescope dependencies before making behavior changes.

## Acceptance gates

A. Correct DRM connector/GPU/120 Hz fixed mode from TTY.
B. All but explicitly accounted-for scanout slots present the
   correct parity; record any missed flip and immediate recovery.
C. Timing under load with a generated stereo pair, then SBS movie,
   then a stereo-capable game.
D. Real 3D Vision emitter phase, pulse duration and optical state
   independently measured and controlled with an explicit timeout
   or otherwise demonstrably safe shutdown.
E. Only then allow a brief human test, stopping immediately if
   there is any flicker or discomfort.

For 23.976 fps movies: 120-Hz stereo output creates 60 L/R pairs
per second, requiring a 2/3-pair cadence; 144-Hz output would be
approximately three pairs per movie frame, but must first be
validated with the actual emitter/monitor at 144 Hz.
