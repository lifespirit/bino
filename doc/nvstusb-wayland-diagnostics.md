# NVIDIA 3D Vision on Qt Wayland: frame lifecycle diagnostic

This branch adds an **experimental** USB IR-emitter bridge using
[libnvstusb](https://github.com/lifespirit/libnvstusb). These diagnostics
are intended for **120 Hz** KDE Plasma Wayland with Bino's
`--output=alternating` stereo mode.

**Important:** Qt's `paintGL()`, `aboutToCompose()`, and `frameSwapped()`
events are **not Wayland `wp_presentation` timestamps**. Neither a 120 Hz
Qt callback rate nor 120 successful USB writes proves that the shutter
glasses are aligned with the monitor's scanned-out frames.

## Build on Arch

From the libnvstusb source tree, make sure the shared library exists:

```sh
cd ~/Projects/src/libnvstusb
git pull --ff-only
make -j"$(nproc)"
ls src/.libs/libnvstusb.so
```

Build Bino from this fork (Qt 6.7 or newer required):

```sh
sudo pacman -S --needed base-devel git cmake ninja qt6-base qt6-multimedia qt6-tools

cd ~/Projects/src/bino
git pull --ff-only
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_NVSTUSB=ON \
  -DNVSTUSB_SOURCE_DIR="$HOME/Projects/src/libnvstusb"
cmake --build build -j"$(nproc)"
```

The USB controller is entirely optional at compile time
(`-DENABLE_NVSTUSB=OFF`), and is never initialized unless `--nvstusb`
is passed at runtime.

## Phase 1: Qt diagnostics WITHOUT opening the emitter

Set the monitor to **120 Hz** and disable VRR while testing.
Close any running `test-usb` or Bino processes before actual USB testing.

Choose the proper input layout for your movie, e.g.
`--input=left-right-half` for half-width side-by-side.

```sh
QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-dry-run --nvstusb-stats \
  "/path/to/stereo-movie.mkv" 2>&1 | tee /tmp/bino-nvstusb-dry.log
```

Use a sufficiently long **stereo** clip (not mono). The dry-run mode does
not load firmware or open USB: it simply tracks Qt's alternating-frame
lifecycle and simulates shutter command submissions.

Every five seconds the program prints:

- `paint`: stereo images drawn in `paintGL`, their observed frequency,
  average/max interval and how many adjacent paints use the same eye
- `compose`: Qt top-level composition starts; `no_new_paint` counts
  compositions without new stereo renders, and `coalesced` counts
  paints superseded by later paints before composition
- `swap`: `QOpenGLWidget::frameSwapped` signal counts/rate, **not**
  physical display presentations
- `callbacks`: selected eye-change trigger frequency and its jitter,
  with `late` measuring callback gaps above 1.5 expected refresh periods
- `commands`: command dispatches; in dry-run these are simulated,
  **not** successful USB transfers

Look for roughly 120 Hz for `paint` and `callbacks` during active
alternating video, with small `late` and `same_eye` counts. If the
rates are much lower, debug Qt/compositor scheduling before any shutter
phase calibration. Some composition events can occur without a video
update; that is why all stages are counted independently.

## Phase 2: compare real USB timing, without changing the player

```sh
QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-stats --nvstusb-rate=120 \
  --nvstusb-trigger=swap --nvstusb-phase=0 \
  --nvstusb-firmware="$HOME/Projects/src/libnvstusb/nvstusb.fw" \
  "/path/to/stereo-movie.mkv" 2>&1 | tee /tmp/bino-nvstusb-usb.log
```

A further run may compare `--nvstusb-trigger=compose` with
`--nvstusb-trigger=swap` and, if needed, `--nvstusb-invert`.
The optional `--nvstusb-phase=0..7` applies a Qt timer delay to
shutter writes; **do not interpret this as calibrated scanout phase**.

**Never run `test-usb` concurrently with Bino's emitter mode**.
Bulk transfers are synchronous on the GUI thread in this experimental
bridge, so a slow USB call can affect UI frame pacing. USB writes have
bounded timeouts via libnvstusb.

## Still missing

Accurate stereoscopic synchronization requires associating the displayed
left/right image with a Wayland surface submission, receiving
`wp_presentation` feedback (including discarded submissions), and
calibrating the latency from frame scanout to the glasses' IR response.
Qt `frameSwapped` alone does not provide this. Even presentation
feedback reports when a buffer became visible, so a full solution
must account for shutter-command lead time and display scanout.

## Results and follow-up: asymmetrical shutter behavior (2026-10-17)

The first 60-second comparisons on an actual 23.98 fps Half-SBS video showed:

- Dry-run Qt callbacks: **97.0–109.6 Hz**, typically near 100 Hz.
- With USB enabled: **96.4–113.2 Hz**, also typically near 100 Hz.
- Both had multi-frame stalls of roughly **16–47 ms**, far above the
  **8.33 ms** refresh period at 120 Hz.
- Qt reported `same_eye=0`, `replaced=0` in these tests. Real USB
  calls were generally under 0.3 ms, so their measured duration does not
  explain the missing ~20 Hz.
- The decoder printed `GPU at BusId ... doesn't have a supported video
  decoder`. This may indicate software decode, but we have **not yet
  established it as the cause** of the frame cadence problem.

The video renderer originally allocated a new full-resolution depth
texture in `Bino::render()` on every paint. The source now reuses that
texture until resolution/context initialization changes. This is a
**performance experiment**, not a proven fix for the physical IR timing.

The statistics now distinguish the two alternating transition directions:

- `LtoR_avg`, `LtoR_max`: interval between left then right callback
- `RtoL_avg`, `RtoL_max`: interval between right then left callback
- `usb_left_max`, `usb_right_max`: worst observed synchronous USB
  write time per eye

Rebuild Bino and rerun **dry-run first**, using the identical screen
mode/movie/scene and at least 30 seconds of steady playback. Compare
`paint`, `late`, `LtoR_avg`, and `RtoL_avg` with the previous run.
Only if dry-run improves, retest with real USB enabled.

Do not assume that equal average transition gaps prove equal physical
shutter duty cycles: the emitter firmware and IR receiver also have
their own timing/state machines. Conversely, observing different
shutter brightness does not prove one lens is broken.

## New-video-frame vs cached-frame performance counters

The next dry-run measurement adds the following fields after
`usb_right_max`:

- `video_new`, `video_cached`: number of Bino GUI paints that did and did
  not upload new media video frame data in `preRenderProcess`
- `pre_new_avg`, `pre_new_max`: CPU wall-clock time of
  `Bino::preRenderProcess` when uploading a new movie frame
- `pre_cached_avg`, `pre_cached_max`: time on a normal cached redraw
- `paint_new_avg`, `paint_new_max`: total synchronous `Widget::paintGL`
  CPU wall-clock time on a new-frame paint
- `paint_cached_avg`, `paint_cached_max`: same when reusing a video frame
- `paint_late_new`, `paint_late_cached`: number of paint-to-paint gaps above
  1.5 refresh periods, classified by whether the **later** paint uploaded
  a new movie frame

These are diagnostic CPU measurements, **not GPU fence durations**; driver
and compositor scheduling can introduce delays outside these boundaries.
A correlation with `video_new` does not prove the upload alone is slow.
If both new and cached paints are fast but the intervals are late,
investigate Qt/KWin's render/composition/pacing and GPU execution.

## Experiment: decouple movie-frame wakeups from 120 Hz alternating redraws

An approximately two-minute dry-run with a 23.98 fps movie showed
approximately 105 Hz Qt paint callbacks despite individual
`paintGL()` CPU measurements generally being below 1 ms.

Of 2755 paints that uploaded a **new** decoded frame, 2669 (96.9%)
followed a paint-to-paint interval longer than 1.5 nominal 120 Hz
periods. Only 362 of 9355 (3.9%) cached-frame paints were late.
This is correlation, not proof that the decoder's `newVideoFrame`
signal causes the missed display refreshes.

By default, two independent sources call `Widget::update()`:
the `newVideoFrame` signal from Qt Multimedia (at roughly 24 Hz)
and the self-scheduling alternating redraw loop inside `paintGL()`.

To isolate these sources, compare two dry-run tests on the same
movie, monitor refresh rate and fullscreen mode:

**Baseline:**

```sh
QT_QPA_PLATFORM=wayland ./build/bino --input=left-right-half \
  --output=alternating --fullscreen --nvstusb --nvstusb-dry-run \
  --nvstusb-stats "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-baseline.log
```

**Experimental suppression:**

```sh
QT_QPA_PLATFORM=wayland ./build/bino --input=left-right-half \
  --output=alternating --fullscreen --nvstusb --nvstusb-dry-run \
  --nvstusb-stats --nvstusb-ignore-video-wakeups \
  "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-no-video-wakeup.log
```

The experimental flag is **dry-run-only** and does not open the
USB emitter. Decoded frames still update Bino's video data and are
uploaded on the next alternating paint. The flag only suppresses
the extra `update()` call triggered by `newVideoFrame`, **after**
the self-sustaining stereo render loop has started. New-media and
non-stereo cases still use normal wakeups.

Compare `paint`, `callbacks`, `paint_late_new`,
`paint_late_cached` and `ignored_video_wakeups`; the last count
should roughly match decoded movie frames received during active
stereo playback. If timing improves, prioritize frame pacing
and a frame-accurate presentation mechanism before changing the
libnvstusb firmware timings. Even if timing does not improve,
Qt presentation timing must still be investigated before claiming
that physical shutters are synchronized with the displayed eyes.

## Finding from suppressed video wakeups (v4)

With `--nvstusb-ignore-video-wakeups`, the Qt render loop still
averaged **103.5 Hz**, with the same dips to approximately **97 Hz**.
The controller counted 1556 suppressed video-frame-triggered repaint
requests, matching 1557 video uploads. Thus suppressing the extra
`Widget::update()` calls did not resolve the timing.

The next question is whether Qt Multimedia's `VideoSink::processNewFrame()`
blocks the GUI thread *before* `paintGL()`. That work was outside all
earlier `paint_new_avg` measurements.

The code now supports optional profiling without changing playback:

```sh
BINO_PROFILE_VIDEO_SINK=1 QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-dry-run --nvstusb-stats \
  --nvstusb-ignore-video-wakeups \
  "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-video-sink-profile.log
```

Every 120 decoder frames (roughly five seconds), a new
`bino: video_sink stats` record reports the wall-clock duration of
the `QVideoSink::videoFrameChanged` handler, including the call to
`VideoFrame::update()` (`handler_avg`, `handler_max`,
`handler_gt1ms`, `gt4ms`, `gt8ms`), plus the
`VideoFrame::update()` stage alone (`update_avg`, `update_max`).

If the handler is consistently short while the Qt render cadence
is late, investigate scheduling between Qt's framebuffer-composition
callbacks and the compositor/presentation feedback rather than assuming
decoding or USB is the cause. If the handler regularly takes several
milliseconds, investigate frame mapping/copying and Qt Multimedia
thread affinity next. These are CPU wall-clock durations, not GPU
timestamps.

## v5: synchronous new-frame handling is the large cost

The `BINO_PROFILE_VIDEO_SINK=1` experiment on a 23.98 fps Half-SBS
video produced 14 sets of 120 input frames (1680 frames total):

- `handler_avg` ~11.65 ms, and virtually all of this time was spent in
  `VideoFrame::update()` (`update_avg` ~11.65 ms)
- 1431/1680 (~85.2%) input-frame processing calls lasted >8 ms
- `paintGL()` was normally <1 ms, and Qt swap callbacks still
  averaged only ~100.6 Hz

This indicates **expensive processing prior to `paintGL()`**.
Within `VideoFrame::update()`, potential blocking points are
unmapping the old `QVideoFrame`, replacing its implicit shared
handle, mapping the new frame with `QVideoFrame::ReadOnly`,
or pixel-format fallback conversion.

To identify the actual operation, update Bino and rerun the identical
dry-run, this time with *both* profiling variables:

```sh
BINO_PROFILE_VIDEO_SINK=1 BINO_PROFILE_VIDEO_FRAME=1 \
QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-dry-run --nvstusb-stats \
  --nvstusb-ignore-video-wakeups \
  "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-dry-v6.log
```

`bino: video_frame stats:` prints a report per 120 calls including:
`mapped`, `images`, `map_failed`, `total_avg/max`,
`unmap_avg/max`, `assign_avg/max`, `map_avg/max`,
`image_avg/max`, and `other_avg`.

`bino: video_sink stats` now additionally reports `gui_thread` so we
can establish whether a slow decoder-frame handler is blocking the Qt
GUI event loop. All timers measure CPU-side wall-clock elapsed time;
the operation timing is observational only. No asynchronous frame
queue or alternative buffer ownership strategy has been enabled yet.

## v6: definitive GUI-thread bottleneck and experimental async mitigation

The `BINO_PROFILE_VIDEO_FRAME=1` test confirmed `QVideoFrame::map(ReadOnly)`
is the dominant contributor to GUI stalls: around **11.06 ms on
average** across thirteen 120-frame reporting windows (1560 frames).
`unmap()` averaged about **0.002 ms** and reference assignment about
**0.012 ms**. `VideoSink` reported `gui_thread=yes` in every window.
Thus the synchronous map blocks the GUI event loop well beyond a
120 Hz monitor's 8.33 ms refresh period.

An **opt-in, experimental** workaround is now available for SBS/TB
and other single-source-frame modes:

```sh
BINO_ASYNC_VIDEO_MAP=1 \
BINO_PROFILE_VIDEO_FRAME=1 QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-dry-run --nvstusb-stats \
  --nvstusb-ignore-video-wakeups \
  "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-dry-v7.log
```

The new worker maps the input frame, performs metadata conversion,
copies the mapped image planes into independent CPU memory, and
unmaps on its worker thread. Completed frames are transferred using a
queued GUI callback; only that callback replaces Bino's displayed
`VideoFrame`. GL uploads continue on the GUI/GL thread but now
use the stored plane strides, not `QVideoFrame::bytesPerLine()` on
an unmapped decoder buffer.

To bound latency and memory, only one frame can be preparing and one
pending input frame is retained: newer input replaces any older
pending frame. Async results from older playlist generations are
discarded. `video_sink async stats` reports delivered frame count
and overwritten pending count. For ordinary 23.98 fps video and
~12 ms worker mapping, pending frame drops are not expected.

This mode is intentionally **off by default** and not yet validated
on the user's Qt/AMD GPU system. Mapping GPU-backed Qt video frames
on another thread is an experiment; monitor for failed mappings,
renderer artifacts, FFmpeg backend compatibility, A/V delay, and
application lifetime issues. Keep the IR emitter disconnected
(`--nvstusb-dry-run`) during the first test. The old input-video
alternating frame-pair modes are excluded from the async path.

Compare `paint` and `paint_late_new` in the v7 log against v6,
as well as `video_sink async stats`. Even 120 Hz Qt frame callbacks
alone do **not** guarantee synchronization with physical scanout.

## v7: first async results (Half-SBS 1080p, 23.98 fps)

With `BINO_ASYNC_VIDEO_MAP=1` enabled, the first 13 reported five-second
windows contain **7699 Qt paint callbacks in approximately 65 seconds**,
or **118.4 Hz** overall, compared with ~100.6 Hz in the preceding
synchronous-map run. Most steady windows reach 119.2--119.8 Hz.
Only **31 of 1556 paints uploading new decoded frames** (~2.0%)
followed a long paint interval; this was close to 99% in the previous
synchronous mode.

The async queue reported **1560 frames delivered and zero overwritten
pending frames**, and no mapping errors were printed. Residual Qt
callback outliers up to ~31 ms remain; this is therefore not yet a
guarantee of physical 120 Hz scanout/shutter alignment.

**Instrumentation correction after v7:** `VideoFrame()` invokes
`update()` once on an invalid fallback frame. The first v7
`video_frame stats` reports therefore included one invalid constructor
call per real decoded frame (`frames=120 mapped=60`), artificially
halving the per-call average mapping time. Commit bc23a06 excludes
invalid fallback frames from this profiler only. Future reports
represent valid input movie frames (`frames=120 mapped=120`), and
the actual per-map averages should be closer to 12--14 ms, **not**
the 6--7 ms diluted averages in v7. The actual worker algorithm,
rendering and ownership rules are unchanged.

The next step is a limited **real emitter** test. Recompile first, then
use the environment variable to enable the worker **without**
`--nvstusb-dry-run` and without the dry-run-only
`--nvstusb-ignore-video-wakeups`:

```sh
BINO_ASYNC_VIDEO_MAP=1 QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-stats \
  "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-usb-async.log
```

This test checks whether the previously measured GUI pacing
improvement survives synchronous USB transfer overhead. It does not
establish optical shutter alignment: Qt `frameSwapped` is not
Wayland physical presentation feedback. Check actual viewing
and left/right shutter timing carefully. Do not claim the shutters
are phase-locked before external timing or presentation measurements.

## v8 investigation: residual compositor/Qt stereo timing glitches

With real USB and asynchronous mapping, `--nvstusb-trigger=swap`
produced ~118.56 Hz average in 22 reporting windows and
115 callback intervals longer than 12.5 ms. `compose` yielded
~118.83 Hz and 99 late intervals in 22 windows (one clean
five-second window only). Longest callback gaps were ~41.24 ms
for swap and ~21.24 ms for compose. USB writes stayed <= 0.4 ms.
The user's physical right-eye shutter still visibly flickers every
5–10 seconds, making 3D uncomfortable. Neither trigger solves it.

**Do not rely on these paths for comfortable viewing.** Qt
`QOpenGLWidget::aboutToCompose` and `frameSwapped` do not expose
Wayland physical presentation timestamps. An application may redraw
at ~120 Hz but still miss a display refresh; the IR emitter is
currently commanded by these Qt callbacks rather than a physical
vblank / refresh sequence. The logs alone do not distinguish an
occasional missed display frame, misphased IR signal, or a problem
with the physical right-eye shutter.

A conservative *dry-run-only* experiment is now available to test
whether preserving the QOpenGLWidget framebuffer contributes:

```sh
BINO_ASYNC_VIDEO_MAP=1 BINO_STEREO_NO_PARTIAL_UPDATE=1 \
QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-dry-run --nvstusb-stats \
  "/path/to/23.98fps-half-sbs.mkv" \
  2>&1 | tee /tmp/bino-no-partial-dry.log
```

Compare against an otherwise identical dry-run without
`BINO_STEREO_NO_PARTIAL_UPDATE=1`. In this experiment the widget
invalidates its FBO after each complete stereo repaint rather than
preserving it. It has **not been confirmed** to improve callback
cadence, and even perfect Qt callback cadence would not prove
physical presentation/shutter synchronization. Do not do prolonged
visual tests with the glasses until this is resolved.

Also make sure KWin/monitor adaptive sync (VRR) is disabled and the
output mode is fixed 120 Hz before measuring frame timing.

**Proper solution direction:** use the Wayland `wp_presentation`
feedback protocol on a true presentation surface. It reports
whether a committed frame was presented or discarded, and reports
a hardware-refresh sequence where available. Align the stereo
display buffer's eye identity with actual presentation sequence
and schedule shutter commands from a clock/refresh-prediction
source. Account explicitly for discarded or repeated frames rather
than toggling an eye on each `paintGL()`. A native fullscreen
QOpenGLWindow avoids QOpenGLWidget's extra QWidget composition, but
it alone does not provide physical presentation confirmation.

## Native fullscreen test: direct EGL surface and Wayland presentation feedback

After the QOpenGLWidget experiments, `kscreen-doctor -o` confirmed
**DP-1, 2560x1440 at exactly 120.00 Hz** with KWin reporting
**`Vrr: incapable`**. A `QOpenGLWidget::NoPartialUpdate` dry run
averaged ~118.6 Hz and still showed 122 late Qt callback intervals
across 22 measurement windows. It did not improve the problem.

The next experimental output is a **standalone native
`QOpenGLWindow`**. It retains Bino's color conversion, stereo
shaders, player controls and asynchronously mapped movie frames,
but renders directly to the EGL surface without the intermediate
`QOpenGLWidget` composition framebuffer. Its alternating loop
schedules the next update from the native `frameSwapped` signal,
not from `paintGL`. Normal `Gui`/`Widget` mode remains unchanged.

First build and test **without opening the USB device and without
wearing the glasses**:

```sh
cd ~/Projects/src/bino-nvstusb
git pull --ff-only origin main
cmake -S . -B build
cmake --build build -j"$(nproc)"

BINO_ASYNC_VIDEO_MAP=1 QT_QPA_PLATFORM=wayland ./build/bino \
  --input=left-right-half --output=alternating --fullscreen \
  --nvstusb --nvstusb-native-window --nvstusb-dry-run --nvstusb-stats \
  "$HOME/Веном_Venom (2018) BDRip.Half-SBS.1080p.NNMClub.mkv" \
  2>&1 | tee /tmp/bino-native-dry.log
```

`Escape` closes the native video window. The main menu and ordinary
QWidget toolbars are not present in this experimental fullscreen mode.
Basic Bino keyboard shortcuts for playback, seeking and eye swapping
remain available.

When building on Linux with **Qt >= 6.9**, `wayland-client`,
`wayland-scanner` and the `presentation-time.xml` from
`wayland-protocols`, CMake automatically generates the native
`wp_presentation` client protocol. Search for the build output
`Experimental Wayland wp_presentation feedback: YES`.
The code uses QtWayland 6.9+'s `QWindow::winId()` semantics
(the returned identifier points at `wl_surface`). It uses the
native `QWaylandApplication` interface for the same Wayland
connection as Qt. On older Qt or other window systems, presentation
feedback is disabled, while the native window can still run.

When supported by KWin, the experiment prints
`bino: wayland-presentation stats:` after 600 compositor replies,
containing:

- `presented`: content updates confirmed as visible
- `discarded`: submitted updates that the compositor did not show
- `skipped_vblanks`: gaps in actual compositor presentation sequence
- `eye_parity_errors`: changes of eye inconsistent with presentation
  sequence parity across successive *presented* movie frames
- `refresh`: compositor reported refresh period (not app scheduling)
- `feedback_avg`: delivery latency from feedback request to callback
  (not the actual presentation latency)
- `pending` and `feedback_overflow`: outstanding responses/diagnostics.

Wayland feedback is sent **after actual presentation**; it cannot
in itself trigger the shutter in time for the just-presented frame.
This initial implementation records the samples only. The USB
still uses the Qt native `frameSwapped` event, and true optical
phase correction is **not yet enabled**. Once native timing and
KWin sequence data are observed, a separately gated predictor
and dropped-frame recovery strategy can be tested.

**Important safety note:** the existing right-lens flicker produces
significant visual discomfort. Do not try to assess long intervals
by wearing the glasses. Benchmark with `--nvstusb-dry-run` first
and inspect the feedback/late counters. Hardware validation,
if warranted, should be brief and stopped immediately if
flicker persists.

The native window and protocol client have not been compiled on the
target Arch machine in this iteration. Compile errors, lack of
`wp_presentation` support, black video, or KWin-specific timing
behavior need to be checked with its first dry-run log.
