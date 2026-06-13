# JONImageProcessor

JONImageProcessor is a C++17 video processing prototype for NVIDIA Jetson Orin Nano. It captures a live camera image through V4L2 or reads a video file for development, generates a person mask with TensorRT, and renders a processed fullscreen image through DRM/KMS or an OpenCV HighGUI window.

## Table of Contents

- [Current Scope](#current-scope)
- [Prerequisites](#prerequisites)
  - [Build Host](#build-host)
  - [Jetson Target](#jetson-target)
- [Build](#build)
  - [Local Development Build](#local-development-build)
  - [Jetson Cross Build](#jetson-cross-build)
- [Deploy To Jetson](#deploy-to-jetson)
- [Run](#run)
  - [Blur Background](#blur-background)
  - [Color Background](#color-background)
  - [Image Background](#image-background)
  - [HighGUI Window](#highgui-window)
  - [Video File Test](#video-file-test)
- [Process Mode](#process-mode)
- [Running As Systemd Service](#running-as-systemd-service)
- [Command Line Options](#command-line-options)
- [JSON Configuration](#json-configuration)
- [Runtime Behavior](#runtime-behavior)
- [Benchmarking](#benchmarking)
- [IPC Control Interface](#ipc-control-interface)
- [Notes](#notes)
- [Architecture](docs/architecture.md)

## Current Scope

Kept runtime features:

- V4L2 camera input.
- OpenCV video file input with `--input` for development and testing.
- TensorRT mask backend.
- DRM/KMS display backend.
- OpenCV HighGUI display backend.
- Background effects: `blur`, `color`, and `image`.
- Benchmark and verbose diagnostics.

Removed runtime features:

- MP4/file output.
- OpenCV camera capture backend.
- Experimental mask backends.
- User-selectable capture backend and display mode.

The camera capture backend is fixed to V4L2. Video files always use OpenCV file capture. Display scaling is fixed to fill the active canvas while preserving aspect ratio and cropping if needed. TensorRT is the only real mask backend.

## Prerequisites

### Build Host

Install on the x86_64 Linux build host:

- Docker with access to NVIDIA NGC images.
- `git`
- `rsync`
- `scp`/OpenSSH client
- `file`

Pull the NVIDIA JetPack cross-compile container:

```bash
docker pull nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

Create a Jetson sysroot on the build host. The sysroot must contain the Jetson target libraries and headers, including OpenCV, CUDA, TensorRT, DRM/KMS, GBM, EGL, and GLES:

```bash
mkdir -p "$HOME/sysroots/orin-nano" && rsync -aHAX --numeric-ids tseiman@jon:/usr "$HOME/sysroots/orin-nano/" && rsync -aHAX --numeric-ids tseiman@jon:/lib "$HOME/sysroots/orin-nano/" && rsync -aHAX --numeric-ids tseiman@jon:/opt "$HOME/sysroots/orin-nano/"
```

Some protected system files may fail with rsync permission errors. That is acceptable as long as the needed development files exist:

```bash
test -d "$HOME/sysroots/orin-nano/usr/include" && test -d "$HOME/sysroots/orin-nano/usr/lib/aarch64-linux-gnu" && find "$HOME/sysroots/orin-nano/usr" -name OpenCVConfig.cmake -o -name opencv4.pc && find "$HOME/sysroots/orin-nano/usr" -name NvInfer.h
```

### Jetson Target

Install or verify on the Jetson:

- JetPack 6.1 compatible runtime.
- OpenCV runtime libraries.
- CUDA/TensorRT runtime libraries.
- V4L2 camera access.
- DRM/KMS access through `/dev/dri/card*`.
- The TensorRT mask model under `~/JONImageProcessor/models/`.

For direct DRM/KMS fullscreen output, stop the graphical display manager before running the DRM backend:

```bash
sudo systemctl stop gdm3
```

If Ubuntu uses another display manager, replace `gdm3` with the active service, for example `lightdm` or `sddm`.

## Build

### Local Development Build

Use this only for syntax checks and HighGUI/file-input development on a Linux VM with OpenCV development files installed:

```bash
cmake -B build -S .
```

```bash
cmake --build build
```

Without `-DJON_ENABLE_TENSORRT_MASK=ON`, the binary can show help/version and run diagnostics with `--no-mask`, but TensorRT masking is not available.

### Jetson Cross Build

Build the AArch64 Jetson binary from the build host:

```bash
ENABLE_TENSORRT_MASK=ON ENABLE_DRM_DISPLAY=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

The output binary is:

```bash
build-jetson-cross/JONImageProcessor
```

Verify the target architecture:

```bash
file build-jetson-cross/JONImageProcessor
```

Expected result: an AArch64/Linux executable.

## Deploy To Jetson

Run this on the build host:

```bash
scp build-jetson-cross/JONImageProcessor tseiman@jon:~/JONImageProcessor/JONImageProcessor
```

Run these commands on the Jetson:

```bash
mkdir -p ~/JONImageProcessor/models
```

```bash
chmod +x ~/JONImageProcessor/JONImageProcessor
```

Copy the TensorRT/ONNX model to the Jetson, for example:

```bash
scp ~/models/modnet_photographic_portrait_matting.onnx tseiman@jon:~/JONImageProcessor/models/
```

## Run

Set the model path on the Jetson:

```bash
MODEL_PATH="$HOME/JONImageProcessor/models/modnet_photographic_portrait_matting.onnx"
```

### Blur Background

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --mask-threshold 0.7 --mask-smoothing 0.65 --mask-morphology light --background-effect blur --blur-strength 85 --display-backend drm --fullscreen --benchmark
```

### Color Background

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --mask-threshold 0.5 --mask-smoothing 0.65 --mask-morphology light --background-effect color --background-overlay-color 0,255,0 --background-overlay-alpha 1.0 --display-backend drm --fullscreen --benchmark
```

### Image Background

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --mask-threshold 0.7 --mask-smoothing 0.65 --mask-morphology light --background-effect image --background-image "$HOME/JONImageProcessor/background.jpg" --display-backend drm --fullscreen --benchmark
```

### HighGUI Window

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend highgui
```

### Video File Test

```bash
./JONImageProcessor --input test.mp4 --mask-model "$MODEL_PATH" --background-effect blur --display-backend highgui
```

## Process Mode

JONImageProcessor now runs as a normal foreground process by default. This is the preferred mode for systemd `Type=simple`: systemd starts the process, keeps it attached, handles restart policy, sends `SIGTERM`, and collects stdout/stderr in the journal.

When `JOURNAL_STREAM` is present, JONImageProcessor writes through syslog so journald receives the correct log priority for INFO, WARNING, ERROR, and BENCH messages.

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend drm --fullscreen --benchmark
```

Legacy self-daemon mode is still available when explicitly requested:

```bash
./JONImageProcessor --daemon --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend drm --fullscreen
```

Stop a legacy daemonized process with `SIGTERM`, for example:

```bash
pkill -TERM JONImageProcessor
```

## Running As Systemd Service

The example unit is in `packaging/systemd/JONImageProcessor.service` and uses systemd `Type=simple`. It does not pass `--daemon` or `--no-daemon`.

Copy files from the build host:

```bash
scp build-jetson-cross/JONImageProcessor tseiman@jon:/tmp/JONImageProcessor
```

```bash
scp packaging/systemd/JONImageProcessor.service tseiman@jon:/tmp/JONImageProcessor.service
```

```bash
scp ~/models/modnet_photographic_portrait_matting.onnx tseiman@jon:/tmp/modnet_photographic_portrait_matting.onnx
```

Install on the Jetson:

```bash
sudo mkdir -p /opt/JONImageProcessor/models
```

```bash
sudo cp /tmp/JONImageProcessor /opt/JONImageProcessor/JONImageProcessor
```

```bash
sudo cp /tmp/modnet_photographic_portrait_matting.onnx /opt/JONImageProcessor/models/
```

```bash
sudo cp /tmp/JONImageProcessor.service /etc/systemd/system/JONImageProcessor.service
```

```bash
sudo systemctl daemon-reload
```

```bash
sudo systemctl enable JONImageProcessor.service
```

```bash
sudo systemctl start JONImageProcessor.service
```

```bash
sudo systemctl status JONImageProcessor.service
```

```bash
journalctl -u JONImageProcessor.service -f
```

## Command Line Options

- `-h`, `--help`: show help.
- `-c`, `--config <path>`: read configuration from JSON file.
- `-t`, `--test-config`: parse and validate configuration, then exit.
- `--version`: show release/git version.
- `--daemon`: detach into legacy self-daemon mode.
- `-n`, `--no-daemon`: run as foreground process; accepted for compatibility because this is now the default.
- `-v`, `--verbose`: enable detailed logs.
- `-i`, `--input <path>`: use a video file as input. Without this option, the V4L2 camera is used.
- `-d`, `--device <path>`: V4L2 camera device. Default: `/dev/video0`.
- `-p`, `--processing-size <WxH>`: processing size and requested camera size. Default: `1920x1080`.
- `-o`, `--output-size <WxH>`: explicit render canvas size. Default: `auto`.
- `--camera-format <MJPG|YUYV>`: requested V4L2 pixel format. Default: `MJPG`.
- `--camera-connect-timeout <seconds>`: seconds to show `Camera connecting...` after runtime camera re-enable before disconnected status. Default: `10`.
- `--mask-model <path>`: TensorRT mask model path. Required unless `--no-mask` is used.
- `-s`, `--segmentation-size <WxH>`: TensorRT input size. Default: `384x384`.
- `--mask-threshold <0.0..1.0>`: foreground threshold. Default: `0.5`.
- `--mask-smoothing <0.0..1.0>`: temporal mask smoothing. Default: `0.65`.
- `--mask-morphology <off|light|strong>`: mask cleanup mode. Default: `light`.
- `--background-effect <color|blur|image>`: background effect. Default: `color`.
- `--background-image <path>`: JPEG or PNG image used by `--background-effect image`.
- `--pause-image <path>`: JPEG or PNG image used for camera status screens.
- `--pause-image-enabled <true|false>`: use pause image instead of generated camera status screens. Default: `false`.
- `--pause-image-status-text <true|false>`: render camera status text over the pause image. Default: `true`.
- `--pause-image-text-color <RRGGBBAA>`: status text color for the pause image overlay. Default: `ffffffff`.
- `--pause-image-text-position <XxY>`: status text position on the pause image. Use `auto` for the default centered position. Default: `auto`.
- `--pause-image-text-size <value>`: status text size on the pause image. Default: `1.6`.
- `--pause-image-font <font>`: status text font on the pause image. Supported: `plain`, `simplex`, `duplex`, `complex`, `triplex`, `complex-small`, `script-simplex`, `script-complex`. Default: `simplex`.
- `--background-overlay-color <R,G,B>`: color used by `--background-effect color`. Ignored for blur/image. Default: `0,255,0`.
- `--background-overlay-alpha <0.0..1.0>`: alpha used by `--background-effect color`. Ignored for blur/image. Default: `0.35`.
- `--blur-strength <1..100>`: blur strength used by `--background-effect blur`. Default: `15`.
- `--display-backend <highgui|drm>`: display backend. Default: `highgui`.
- `--fullscreen`: request fullscreen display output.
- `--benchmark`: collect benchmark statistics for IPC. Benchmark log output is only written when `--verbose` is also enabled.
- `--no-display`: disable display output.
- `--no-mask`: disable TensorRT mask generation.
- `--no-overlay`: disable background effect rendering.
- `--ipc-socket <path>`: Unix domain socket path for runtime control. Use `none` to disable IPC. Default: `/tmp/jonimageprocessor.sock`.

## JSON Configuration

JONImageProcessor can read grouped JSON configuration before parsing CLI options. CLI options always override JSON values. If no config is provided and no default config exists, built-in defaults are used.

Default filename: `jonimageprocessor.json`

Default search order:

- `/etc/jonimageprocessor.json`
- `jonimageprocessor.json` next to the executable

Example config:

```bash
./JONImageProcessor --config ./etc/jonimageprocessor.json
```

CLI override example:

```bash
./JONImageProcessor --config ./etc/jonimageprocessor.json --mask-threshold 0.6
```

Config test example:

```bash
./JONImageProcessor --test-config --config ./etc/jonimageprocessor.json
```

The project example is [etc/jonimageprocessor.json](/home/tseiman/agent-work/JONImageProcessor/etc/jonimageprocessor.json).

Supported JSON groups:

```json
{
  "camera": {
    "device": "/dev/video0",
    "format": "MJPG",
    "connectTimeoutSeconds": 10
  },
  "processing": {
    "size": "1920x1080"
  },
  "segmentation": {
    "size": "384x384",
    "maskModel": "/opt/JONImageProcessor/models/model.engine",
    "threshold": 0.5,
    "smoothing": 0.65,
    "morphology": "light"
  },
  "background": {
    "effect": "color",
    "image": "/opt/JONImageProcessor/backgrounds/background.png",
    "overlayColor": "0,255,0",
    "overlayAlpha": 0.35,
    "blurStrength": 15
  },
  "pause": {
    "enabled": false,
    "image": "testdata/sample_pause.jpg",
    "showStatusText": true,
    "textColor": "ffffffff",
    "textPosition": "auto",
    "textSize": 1.6,
    "font": "simplex"
  },
  "output": {
    "size": "auto"
  },
  "ipc": {
    "socket": "/tmp/jonimageprocessor.sock"
  },
  "display": {
    "backend": "drm",
    "mode": "fullscreen"
  },
  "diagnostics": {
    "benchmark": false
  }
}
```

All fields are optional. Unknown JSON fields log warnings and are ignored. Invalid JSON, invalid types, and invalid values stop startup with an error. JSON syntax errors include line and column information where possible. `camera.connectTimeoutSeconds` controls how long `Camera connecting...` is shown after runtime camera re-enable before falling back to `Camera DISCONNECTED`. `pause.enabled` switches camera status screens from the generated pattern to `pause.image`; `pause.showStatusText` controls whether the status label is rendered over that image. `pause.textColor` uses `RRGGBBAA` hex, for example `ffffff0a`. `pause.textPosition` uses `XxY` or `auto`; `pause.textSize` controls the rendered text scale. `pause.font` uses a fixed OpenCV Hershey font name, not a dynamic operating-system font list. `diagnostics.benchmark` enables benchmark collection for IPC without passing `--benchmark`.

## Runtime Behavior

Camera input always uses V4L2 and low-latency capture. The capture thread keeps only the newest frame, so old frames are overwritten instead of queued. If the USB camera disappears, JONImageProcessor closes the broken capture path, renders a `Camera DISCONNECTED` test image, and periodically retries the configured device after it has been visible for a short settle period. Reconnect is accepted after the reopened V4L2 device delivers a valid frame. If `camera.enabled` was set to false through IPC, the application renders `Camera OFF`. When camera input is enabled again, `Camera connecting...` is shown while the camera is being reopened; only if `/dev/video0` is still unavailable after `camera.connectTimeoutSeconds` does the status change to `Camera DISCONNECTED`. If the Jetson kernel does not recreate `/dev/video0` after a USB reconnect, JONImageProcessor continues showing `Camera DISCONNECTED`; a USB/controller reset or service restart may still be required.

Video file input uses OpenCV file capture and processes frames sequentially.

Display mode is fixed to fill. The image fills the output canvas while preserving aspect ratio. Cropping is allowed; stretching is not used. If the DRM/KMS display is not connected during service startup, JONImageProcessor stays alive and retries display initialization periodically. Camera capture is not started while the display is unavailable.

## Benchmarking

Use `--benchmark` to collect timing statistics for capture, resize, TensorRT preprocessing, TensorRT inference, postprocessing, mask upscale, background effect, display, and total frame time. The values can be read through IPC. Add `--verbose` when benchmark progress and shutdown summaries should be written to the log.

For pipeline timing without display or effects:

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --no-display --no-mask --no-overlay --benchmark
```

Stop long-running foreground benchmarks with `Ctrl-C`. SIGINT is handled cleanly; the final benchmark summary is logged only in verbose mode.

## IPC Control Interface

JONImageProcessor exposes a small Unix domain socket control interface for runtime parameters. The default socket is `/tmp/jonimageprocessor.sock`; use `--ipc-socket <path>` to change it or `--ipc-socket none` to disable IPC. The protocol is NDJSON: send one JSON request line and receive one JSON response line.

Commands:

- `get`: read one key.
- `set`: update one writable key.
- `list`: read all runtime keys.

Writable keys:

- `camera.enabled`: boolean. When false, camera capture stops and a generated `Camera OFF` test image is rendered.
- `pause.enabled`: boolean. When true, camera status screens use `pause.image`.
- `pause.image`: string path.
- `pause.showStatusText`: boolean. When false, no status text is rendered over the pause image.
- `pause.textColor`: `RRGGBBAA` hex color for the pause image status text.
- `pause.textPosition`: `XxY` or `auto`.
- `pause.textSize`: float `0.1..10.0`.
- `pause.font`: one of `plain`, `simplex`, `duplex`, `complex`, `triplex`, `complex-small`, `script-simplex`, `script-complex`.
- `segmentation.threshold`: float `0.0..1.0`
- `segmentation.smoothing`: float `0.0..1.0`
- `segmentation.morphology`: `off`, `light`, `strong`
- `background.effect`: `color`, `blur`, `image`
- `background.image`: string path
- `background.overlayColor`: `R,G,B`
- `background.overlayAlpha`: float `0.0..1.0`
- `background.blurStrength`: integer `1..100`
- `runtime.noMask`: boolean
- `runtime.noOverlay`: boolean

The older flat key names such as `mask_threshold` and `background_effect` are still accepted for compatibility. `list` returns grouped JSON matching the configuration shape for runtime-adjustable values.

Read-only key:

- `benchmark`: current benchmark snapshot. This key is available only when benchmark mode is enabled with `--benchmark` or `diagnostics.benchmark`.

Examples:

```bash
echo '{"cmd":"get","key":"segmentation.threshold"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"set","key":"segmentation.threshold","value":0.6}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"list"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"get","key":"benchmark"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"set","key":"camera.enabled","value":false}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

Invalid JSON, unknown commands, unknown keys, invalid value types, invalid ranges, disabled benchmark reads, and attempts to set `benchmark` return `{"ok":false,...}`. There is intentionally no shutdown command.

## Notes

The current production direction is V4L2 camera input, TensorRT masking, and DRM/KMS fullscreen output. Later systemd service integration should start this binary automatically after boot and open the DRM fullscreen output directly.
