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
