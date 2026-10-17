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
