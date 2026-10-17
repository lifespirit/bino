# Experimental NVIDIA 3D Vision USB on KDE Wayland

This fork integrates the original NVIDIA 3D Vision USB IR emitter
(`0955:0007`) into Bino's `--output=alternating` mode on Linux.
No NVIDIA GPU or NVIDIA display driver is required.

**Status: early hardware success, intermittent eye/display desynchronization.**
The user confirmed perceivable stereo 3D using this fork with the original
NVIDIA USB IR emitter and Samsung Odyssey G7 under KDE Wayland (October 2026).
Synchronization is still intermittent; neither USB success nor the visible
3D effect establishes frame-accurate scanout/shutter timing.
Neither `QOpenGLWidget::frameSwapped` nor the optional Qt delay
guarantees correspondence with KWin's actual monitor scanout. The firmware
must be supplied separately; no proprietary firmware is committed here.

## Prerequisites (Arch Linux)

Build the companion patched [libnvstusb](https://github.com/lifespirit/libnvstusb)
that provides `nvstusb_send_eye()` (not present upstream):

```sh
cd ~/Projects/src/libnvstusb
git fetch origin
# Protect any modified files before updating:
git stash push -u -m 'local USB work before Bino integration'
git merge --ff-only origin/main
make -C src -j"$(nproc)"
nm -D src/.libs/libnvstusb.so | grep nvstusb_send_eye
```

The binary depends on `nvstusb.fw` at runtime when the emitter powers up
without firmware. Keep the firmware outside Git. Prefer a direct USB port:
the nested hubs in our initial setup caused USB I/O errors and timeouts.

Build this fork without replacing an AUR-installed Bino:

```sh
sudo pacman -S --needed cmake ninja pkgconf qt6-base qt6-multimedia qt6-tools
cd ~/Projects/src
git clone https://github.com/lifespirit/bino.git bino-nvstusb
cd bino-nvstusb
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DENABLE_NVSTUSB=ON \
  -DNVSTUSB_SOURCE_DIR="$HOME/Projects/src/libnvstusb"
cmake --build build -j"$(nproc)"
```

Alternatively, install patched libnvstusb and omit `-DNVSTUSB_SOURCE_DIR`;
CMake will use `pkg-config libnvstusb`. The default
`-DENABLE_NVSTUSB=OFF` builds unmodified Bino without the emitter dependency.

## Playback test

Set Samsung Odyssey G7 to exactly 120 Hz in KDE and keep VRR disabled:

```sh
kscreen-doctor output.DP-1.mode.3
```

Run Full-SBS movie:

```sh
cd ~/Projects/src/bino-nvstusb
QT_QPA_PLATFORM=wayland \
LD_LIBRARY_PATH="$HOME/Projects/src/libnvstusb/src/.libs${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
./build/bino \
  --input=left-right --output=alternating --fullscreen \
  --nvstusb --nvstusb-rate=120 --nvstusb-phase=0 \
  --nvstusb-firmware="$HOME/Projects/src/libnvstusb/nvstusb.fw" \
  "/path/to/full-sbs-movie.mkv"
```

Optional: `--nvstusb-invert` swaps the two shutters.
`--nvstusb-trigger=swap` (default) sends commands after Qt composition.
`--nvstusb-trigger=compose` sends commands just before Qt composition,
which may improve or worsen alignment depending on the KWin/display path.
`--nvstusb-stats` prints five-second summaries of callback frequency,
late callbacks, duplicate-eye events and maximum USB write latency.
Use these metrics to compare trigger modes, not as proof of scanout timing.
`--nvstusb-phase=N` delays shutter commands 0-7 **integer milliseconds**
after the Qt frameSwapped signal, using a Qt PreciseTimer. This delay is
**not** a measured phase shift relative to physical vblank. Try 0 first.
The emitter refresh rate should match the **actual monitor refresh**.
The Bino movie file must be interpreted correctly (e.g. Full-SBS is
`--input=left-right`, not `left-right-half`).

## Current implementation

- `Widget::paintGL()` captures which eye the renderer has actually drawn
  **before** advancing its alternating-view state.
- A callback selected by `--nvstusb-trigger` (`aboutToCompose` or
  `frameSwapped`) notifies the USB controller of that eye. The eye is
  consumed once, so later unrelated Qt composition signals cannot resend it.
- Controller sends a direct shutter command through `nvstusb_send_eye()`.
  It neither uses GLX nor a separate free-running 120 Hz thread.
- USB failure disables the emitter and prints an error on standard error.
- USB transfers currently execute on the GUI thread and can stall rendering
  for up to the libnvstusb timeout (1 second on a failed transfer).
- Qt and KWin can drop/merge frames, and the display may scan out long after
  the Qt callback. Consequently, full 3D lock is **not guaranteed**.

## Compare composition triggers

At 120 Hz and VRR disabled, run two otherwise identical Full-SBS tests
while keeping the transmitter directly attached to the laptop:

```sh
# Existing behavior
./build/bino --input=left-right --output=alternating --fullscreen \
  --nvstusb --nvstusb-rate=120 --nvstusb-trigger=swap \
  --nvstusb-phase=0 --nvstusb-stats \
  --nvstusb-firmware="/path/to/nvstusb.fw" "/path/to/movie.mkv"

# Earlier command relative to Qt buffer composition
./build/bino --input=left-right --output=alternating --fullscreen \
  --nvstusb --nvstusb-rate=120 --nvstusb-trigger=compose \
  --nvstusb-phase=0 --nvstusb-stats \
  --nvstusb-firmware="/path/to/nvstusb.fw" "/path/to/movie.mkv"
```

Compare perceived desynchronization and the printed `bino: nvstusb stats:`
lines. For 120 Hz, an ideal interval is about 8.33 ms.
A callback gap over 12.5 ms is logged as `late`, **not** a proven
dropped display frame. Large `usb_max` means synchronous USB writes
are blocking GUI processing. The phase delay is still approximate.


## Observed timing on KDE Wayland, October 17, 2026

The user confirmed **actual stereoscopic depth** with the original shutter
glasses, with intermittent temporary loss of synchronization. The 120 Hz
monitor and direct USB connection were used for both tests.

From the initial five-second telemetry windows:

| Trigger | Windows | Callbacks / s | Late callbacks / 5s | Largest interval | Longest USB OUT |
| --- | ---: | ---: | ---: | ---: | ---: |
| compose | 12 (60 s) | 116.52 | 16.0 average | 26.63 ms | 0.36 ms |
| swap | 6 (30 s) | 115.67 | 16.5 average | 25.55 ms | 0.32 ms |

Both reported zero repeated-eye callbacks and zero replaced pending shutter
commands. The subjective observation is that `compose` lost synchronization
less often, but the objective callback rates are close. These metrics do
**not** measure physical presentation timestamps, actual scanout, nor a
precise count of dropped monitor refreshes.

Bino's current alternating mode advances eye parity for each `paintGL()`.
If one or more 120 Hz presentation opportunities are missed, the parity of
the next *actual* scanout may not match that simple event counter. This is
a likely hypothesis to investigate, not yet a demonstrated root cause.
Improving parity recovery would require a refresh-clock or compositor-
supplied presentation timeline.

The source file used in this experiment was 1920x1080 H.264 at 23.98 fps,
with SAR 2:1, stereo metadata `left_right`, and a filename containing
Half-SBS. Its **display** aspect ratio was reported as 32:9 for the packed
stereo pair, and the tested, visually correct Bino setting was
`--input=left-right`. The format must not be inferred from filename alone.

### Decoder backend A/B test

Both logs also printed:

```text
Hardware is lacking required capabilities
Failed setup for format cuda: hwaccel initialisation returned error.
```

This suggests a failed CUDA hardware-decoder attempt; it does **not**
establish that decoding failures are responsible for ongoing jitter.
Qt Multimedia documents an environment override that can prioritize
VA-API (on supported Mesa/Intel/AMD hardware) or disable all hardware
decoding to compare performance:

```sh
# Try a VA-API decoder:
QT_FFMPEG_DECODING_HW_DEVICE_TYPES=vaapi ./build/bino ...

# Disable all FFmpeg hardware decoding for a control test:
QT_FFMPEG_DECODING_HW_DEVICE_TYPES=, ./build/bino ...
```

Repeat the *same* scene, 120 Hz display mode, `compose` trigger,
`--nvstusb-phase=0`, and `--nvstusb-stats` for 60 seconds. Compare
callback Hz, late callbacks and maximum interval along with the visual
stereoscopic stability. Do not change video mode or phase at the same time.

See Qt's documentation:
https://doc.qt.io/qt-6/advanced-ffmpeg-configuration.html

## Planned follow-up

Measure effective alternation and shutter phase, detect skipped frames,
then implement presentation timestamps using Wayland `wp_presentation`
(or a suitable renderer/display-specific mechanism) rather than relying
solely on Qt's frameSwapped signal. Consider a worker thread for USB output.

The firmware is NVIDIA proprietary material and intentionally omitted.
