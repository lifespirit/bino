# Experimental NVIDIA 3D Vision USB on KDE Wayland

This fork integrates the original NVIDIA 3D Vision USB IR emitter
(`0955:0007`) into Bino's `--output=alternating` mode on Linux.
No NVIDIA GPU or NVIDIA display driver is required.

**Status: experimental prototype, not validated on hardware yet.**
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
`--nvstusb-phase=N` delays shutter commands 0-7 **integer milliseconds**
after the Qt frameSwapped signal, using a Qt PreciseTimer. This delay is
**not** a measured phase shift relative to physical vblank. Try 0 first.
The emitter refresh rate should match the **actual monitor refresh**.
The Bino movie file must be interpreted correctly (e.g. Full-SBS is
`--input=left-right`, not `left-right-half`).

## Current implementation

- `Widget::paintGL()` captures which eye the renderer has actually drawn
  **before** advancing its alternating-view state.
- `QOpenGLWidget::frameSwapped` notifies the USB controller of that eye.
- Controller sends a direct shutter command through `nvstusb_send_eye()`.
  It neither uses GLX nor a separate free-running 120 Hz thread.
- USB failure disables the emitter and prints an error on standard error.
- USB transfers currently execute on the GUI thread and can stall rendering
  for up to the libnvstusb timeout (1 second on a failed transfer).
- Qt and KWin can drop/merge frames, and the display may scan out long after
  the Qt callback. Consequently, full 3D lock is **not guaranteed**.

## Planned follow-up

Measure effective alternation and shutter phase, detect skipped frames,
then implement presentation timestamps using Wayland `wp_presentation`
(or a suitable renderer/display-specific mechanism) rather than relying
solely on Qt's frameSwapped signal. Consider a worker thread for USB output.

The firmware is NVIDIA proprietary material and intentionally omitted.
