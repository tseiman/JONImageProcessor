<img src="docs/logo.svg" align="right" width="72" height="72" alt="JIP logo">

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
- Background effects: `none`, `blur`, `color`, and `image`.
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

Create a Jetson sysroot on the build host. The sysroot must contain the Jetson target libraries and headers, including OpenCV, CUDA, TensorRT, DRM/KMS, GBM, EGL, GLES, FreeType for TTF pause text, and optionally WPE WebKit for HTML media:

```bash
mkdir -p "$HOME/sysroots/orin-nano" && rsync -aHAX --numeric-ids tseiman@jon:/usr "$HOME/sysroots/orin-nano/" && rsync -aHAX --numeric-ids tseiman@jon:/lib "$HOME/sysroots/orin-nano/" && rsync -aHAX --numeric-ids tseiman@jon:/opt "$HOME/sysroots/orin-nano/"
```

Some protected system files may fail with rsync permission errors. That is acceptable as long as the needed development files exist:

```bash
test -d "$HOME/sysroots/orin-nano/usr/include" && test -d "$HOME/sysroots/orin-nano/usr/lib/aarch64-linux-gnu" && find "$HOME/sysroots/orin-nano/usr" -name OpenCVConfig.cmake -o -name opencv4.pc && find "$HOME/sysroots/orin-nano/usr" -name NvInfer.h
```

For HTML background or pause media, install WPE runtime and development files on the Jetson first and sync the sysroot again. The runtime libraries are needed to run; the development files are needed because the sysroot is used for cross-compilation:

```bash
sudo apt-get install libwpewebkit-1.1-0 libwpewebkit-1.1-dev libwpebackend-fdo-1.0-1 libwpebackend-fdo-1.0-dev
```

For TTF pause text rendering, install FreeType development files on the Jetson and sync the sysroot again:

```bash
sudo apt-get install libfreetype-dev
```

### Jetson Target

Install or verify on the Jetson:

- JetPack 6.1 compatible runtime.
- OpenCV runtime libraries.
- CUDA/TensorRT runtime libraries.
- V4L2 camera access.
- DRM/KMS access through `/dev/dri/card*`.
- WPE WebKit runtime libraries if HTML media files are used.
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

Optional AirPlay diagnostics:

- `-DJON_ENABLE_AIRPLAY_H264_DEBUG_DUMP=ON` adds a debug branch to the AirPlay RTP pipeline and writes `/tmp/jon-airplay-debug.h264`.
- The option is `OFF` by default so normal builds do not pay the extra `tee/filesink` overhead.
- For cross builds, set `ENABLE_AIRPLAY_H264_DEBUG_DUMP=ON` before `scripts/build-jetson-cross.sh`.

### Jetson Cross Build

Build the AArch64 Jetson binary from the build host:

```bash
ENABLE_TENSORRT_MASK=ON ENABLE_DRM_DISPLAY=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

The cross-build script installs WPE development packages inside the container when available. HTML support is only enabled when the Jetson sysroot also contains the matching AArch64 WPE WebKit and WPEBackend-fdo headers, libraries, and pkg-config files. The output binary is:

```bash
build-jetson-cross/JONImageProcessor
```

Container apt downloads are cached on the build host under `~/.cache/jonimageprocessor-cross` by default. Use `CROSS_BUILD_CACHE_DIR=/path/to/cache` to move the cache, or `CROSS_BUILD_CACHE_DIR= ./scripts/build-jetson-cross.sh` to disable it.

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
- `--version`: show the release version for exact release-tag builds; otherwise show the 7-character dev git version.
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
- `--mask-threshold <0.0..1.0>`: foreground threshold. A lower internal hysteresis threshold is used to keep weak person regions from disappearing immediately. Default: `0.5`.
- `--mask-smoothing <0.0..1.0>`: temporal mask smoothing. Default: `0.65`.
- `--mask-morphology <off|light|strong>`: mask cleanup mode. `light` and `strong` grow and feather the foreground mask to reduce over-cut body edges. Default: `light`.
- `--background-effect <none|color|blur|image|camera>`: background effect. `none` passes the camera frame through without background replacement. `camera` uses the AirPlay/secondary-camera RTP input as an aspect-ratio-preserved replacement background. Default: `color`.
- `--background-image <path>`: image, video, or HTML file used by `--background-effect image`. JPEG/PNG are loaded as static images; video files are decoded with OpenCV; HTML/CSS/JavaScript is rendered through WPE WebKit when the binary was built with WPE support.
- `--background-image-folder <path>`: base folder for background images selected through IPC. Default: `.`.
- `--background-loop-if-video <true|false>`: loop the background media when it is a video. Default: `false`.
- `--pause-source <image|camera>`: pause media source. `image` uses `--pause-image`; `camera` uses the AirPlay RTP secondary camera input. Default: `image`.
- `--secondary-camera-rtp-port <port>`: UDP RTP port for AirPlay secondary camera input. Default: `5004`. Run uxplay with `/usr/local/bin/uxplay -nc -n "JONImageProcessor v9" -fps 20 -vrtp "config-interval=-1 ! udpsink host=127.0.0.1 port=5004" -as 0 -reset 0 -nohold`.
- `--pause-image <path>`: image, video, or HTML file used for camera status screens when `--pause-image-enabled true` is set. JPEG/PNG are loaded as static images; video files are decoded with OpenCV; HTML/CSS/JavaScript is rendered through WPE WebKit when the binary was built with WPE support.
- `--pause-image-folder <path>`: base folder for pause images selected through IPC. Default: `.`.
- `--pause-loop-if-video <true|false>`: loop the pause media when it is a video. Default: `false`.
- `--pause-image-enabled <true|false>`: use `--pause-image` instead of generated camera status screens while the camera is off, connecting, or disconnected. Default: `false`.
- `--pause-image-status-text <true|false>`: render camera status text over the pause image. Default: `true`.
- `--pause-image-text-color <RRGGBBAA>`: status text color for the pause image overlay. Default: `ffffffff`.
- `--pause-image-text-position <XxY>`: status text position on the pause image. Use `auto` for the default centered position. Default: `auto`.
- `--pause-image-text-size <value>`: status text size on the pause image. Default: `1.6`.
- `--pause-image-font <font>`: status text font on the pause image. Supported built-ins: `plain`, `simplex`, `duplex`, `complex`, `triplex`, `complex-small`, `script-simplex`, `script-complex`. Other safe names load `<font>.ttf` from `--pause-image-font-directory` when the binary was built with FreeType support. Default: `simplex`.
- `--pause-image-font-directory <path>`: directory for TTF pause image fonts. Default: `.`.
- `--pause-image-font-align <left|center|right>`: status text alignment. The configured X position is interpreted as the left edge, center, or right edge. Default: `left`.
- `--pause-preserve-aspect-ratio <true|false>`: letterbox secondary camera pause frames instead of stretching them. Default: `true`.
- `--background-overlay-color <R,G,B>`: color used by `--background-effect color`. Ignored for none/blur/image/camera. Default: `0,255,0`.
- `--background-overlay-alpha <0.0..1.0>`: alpha used by `--background-effect color`. Ignored for none/blur/image/camera. Default: `0.35`.
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
  "configDirectory": "/opt/JONImageProcessor/etc/overlays",
  "camera": {
    "enabled": true,
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
    "folder": "/opt/JONImageProcessor/backgrounds",
    "loopIfVideo": false,
    "overlayColor": "0,255,0",
    "overlayAlpha": 0.35,
    "blurStrength": 15
  },
  "secondaryCamera": {
    "rtpPort": 5004
  },
  "pause": {
    "enabled": false,
    "source": "image",
    "image": "sample_pause.jpg",
    "folder": "testdata",
    "loopIfVideo": false,
    "showStatusText": true,
    "textColor": "ffffffff",
    "textPosition": "auto",
    "textSize": 1.6,
    "font": "simplex",
    "fontDirectory": "/opt/JONImageProcessor/var/fonts",
    "fontAlign": "left",
    "preserveAspectRatio": true
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

All fields are optional. CLI options override JSON values.

Validation rules:

- Unknown JSON fields log warnings and are ignored.
- Invalid JSON, invalid types, and invalid values stop startup.
- JSON syntax errors include line and column information where possible.
- `--test-config` warns about missing referenced files.
- Normal startup fails when an active model, media file, media folder, or configured TTF pause font is missing.

Runtime media paths:

- IPC image updates must be relative names under `background.folder` or `pause.folder`.
- Absolute paths and `..` traversal are rejected.
- IPC image updates validate readability and return `{"ok":false,...}` on failure.
- `background.loopIfVideo` and `pause.loopIfVideo` loop OpenCV-detected video files.
- HTML media is detected from file content and requires a WPE-enabled build.

Runtime overlays:

- `configDirectory` is read only through IPC and is loaded only from the main JSON file.
- Overlay files are optional, use the same grouped JSON shape, and are named `<name>.json`.
- IPC key `config` applies an overlay live without restarting the service.
- Valid config names contain only letters, digits, `_`, and `-`.
- Invalid or missing overlay files leave the current runtime configuration unchanged.
- Startup-only values in overlays are parsed but do not reinitialize display, TensorRT, IPC, processing size, camera device, or secondary camera RTP port.

Pause and AirPlay behavior:

- `camera.enabled` is the JSON equivalent of IPC key `camera.enabled`; `camera.enable` is accepted as a compatibility spelling.
- `pause.source=image` uses the configured pause image/video/HTML media.
- `pause.source=camera` uses AirPlay RTP from `secondaryCamera.rtpPort`.
- `background.effect=camera` uses the same AirPlay RTP input as an aspect-ratio-preserved replacement background behind the masked primary camera foreground.
- Run uxplay with:

```bash
/usr/local/bin/uxplay -nc -n "JONImageProcessor v9" -fps 20 \
  -vrtp "config-interval=-1 ! udpsink host=127.0.0.1 port=5004" \
  -as 0 -reset 0 -nohold
```

The AirPlay worker keeps UDP/RTP reception and H264 parsing alive. It rebuilds only
the NVIDIA decoder pipeline on H264 caps changes, resumes from cached IDR keyframes,
and can restart the decoder on matching IDR frames while running. If AirPlay cannot
be opened or decoded, `Camera 2. DISCONNECTED` is shown and repeated status logs are
rate-limited. Secondary camera frames are letterboxed when their aspect ratio differs
from the output canvas.

Pause text:

- `pause.enabled` switches camera status screens from the generated pattern to `pause.image` or AirPlay camera.
- `pause.showStatusText` controls whether the status label is rendered over pause media.
- `pause.textColor` uses `RRGGBBAA` hex, for example `ffffff0a`.
- `pause.textPosition` uses `XxY` or `auto`; `pause.textSize` controls scale.
- `pause.font` accepts built-in OpenCV Hershey names or a safe TTF base name.
- TTF names must not contain `/`, `\`, or `..`; `<name>.ttf` is loaded from `pause.fontDirectory` when FreeType support is available.
- `pause.fontAlign` accepts `left`, `center`, or `right`.

`diagnostics.benchmark` enables benchmark collection for IPC without passing `--benchmark`.

## Runtime Behavior

Camera input:

- The live camera path always uses V4L2 and low-latency capture.
- The capture thread keeps only the newest frame; old frames are overwritten, not queued.
- If the USB camera disappears, the broken capture path is closed and `Camera DISCONNECTED` is rendered.
- Reconnect is retried after the device node has been visible for a short settle period.
- Reconnect is accepted only after the reopened V4L2 device delivers a valid frame.

Runtime pause:

- Setting `camera.enabled=false` keeps an already-open V4L2 device alive and drains frames.
- The pipeline renders `Camera OFF` instead of processing the live frame.
- Re-enabling the camera reuses the open stream when possible.
- If the device was unavailable, `Camera connecting...` is shown until reconnect succeeds or `camera.connectTimeoutSeconds` expires.

Status screens:

- With `pause.enabled=false`, camera status screens use the generated test pattern.
- With `pause.enabled=true`, status screens use `pause.image` or AirPlay camera depending on `pause.source`.
- If the Jetson kernel does not recreate `/dev/video0` after USB reconnect, JONImageProcessor keeps showing `Camera DISCONNECTED`; a USB/controller reset or service restart may still be required.

Video file input uses OpenCV file capture and processes frames sequentially.

Display mode is fixed to fill. The image fills the output canvas while preserving aspect ratio. Cropping is allowed; stretching is not used. If the DRM/KMS display is not connected during service startup, JONImageProcessor stays alive and retries display initialization periodically. Camera capture is not started while the display is unavailable.

## Benchmarking

Use `--benchmark` to collect timing statistics for capture, resize, TensorRT preprocessing, TensorRT inference, postprocessing, mask upscale, background effect, display, total frame time, process CPU, and process memory. The values can be read through IPC. Add `--verbose` when benchmark progress and shutdown summaries should be written to the log.

TensorRT segmentation preprocessing preserves the source aspect ratio with letterboxing instead of stretching the camera frame into the model input. The RGB input is normalized to `[-1..1]`, matching MODNet-style matting models. The padding is removed from the model output before the mask is composited back onto the processed frame. This keeps body proportions closer to the camera image at `384x384` model sizes.

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

Runtime keys:

- `config`: overlay configuration name without `.json`. The file is loaded from read-only `configDirectory` and applied immediately over the current runtime configuration.
- `camera.enabled`: boolean. When false, live camera processing is paused and a `Camera OFF` status screen is rendered. If the camera is already open, the V4L2 device remains open to avoid a close/reopen cycle.
- `pause.enabled`: boolean. When true, camera status screens use `pause.image`; when false, they use the generated test pattern.
- `pause.source`: `image` or `camera`.
- `secondaryCamera.rtpPort`: read-only UDP RTP port used when `pause.source=camera` or `background.effect=camera`.
- `pause.image`: relative image name under `pause.folder` when set through IPC.
- `pause.folder`: read-only base folder used when `pause.image` is set through IPC.
- `pause.loopIfVideo`: boolean. Loop `pause.image` when OpenCV detects it as video.
- `pause.showStatusText`: boolean. When false, no status text is rendered over the pause image.
- `pause.textColor`: `RRGGBBAA` hex color for the pause image status text.
- `pause.textPosition`: `XxY` or `auto`.
- `pause.textSize`: float `0.1..10.0`.
- `pause.font`: built-in font name or safe TTF base name loaded as `<name>.ttf` from `pause.fontDirectory`.
- `pause.fontDirectory`: read-only TTF font directory.
- `pause.fontAlign`: `left`, `center`, `right`.
- `pause.preserveAspectRatio`: boolean. When true, secondary camera pause frames are letterboxed instead of stretched.
- `segmentation.threshold`: float `0.0..1.0`
- `segmentation.smoothing`: float `0.0..1.0`
- `segmentation.morphology`: `off`, `light`, `strong`
- `background.effect`: `none`, `color`, `blur`, `image`, `camera`
- `background.image`: relative image name under `background.folder` when set through IPC.
- `background.folder`: read-only base folder used when `background.image` is set through IPC.
- `background.loopIfVideo`: boolean. Loop `background.image` when OpenCV detects it as video.
- `background.overlayColor`: `R,G,B`
- `background.overlayAlpha`: float `0.0..1.0`
- `background.blurStrength`: integer `1..100`
- `runtime.noMask`: boolean
- `runtime.noOverlay`: boolean

The older flat key names such as `mask_threshold` and `background_effect` are still accepted for compatibility. `list` returns grouped JSON matching the configuration shape for runtime-adjustable values.

Read-only key:

- `configDirectory`: directory containing IPC-selectable overlay configuration files.
- `system.configDirectory`: same value as grouped system field.
- `system.version`: current binary version string.
- `secondaryCamera.rtpPort`: read-only UDP RTP port used by the AirPlay RTP secondary camera mode for pause or background camera effects.
- `benchmark`: current benchmark snapshot. This key is available only when benchmark mode is enabled with `--benchmark` or `diagnostics.benchmark`. It includes frame counters/timing, accumulated process CPU (`cpu_total_seconds`, `cpu_percent`) and process RSS (`memory_rss_bytes`, `memory_peak_rss_bytes`).

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

```bash
echo '{"cmd":"set","key":"pause.source","value":"camera"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"get","key":"secondaryCamera.rtpPort"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"set","key":"config","value":"meeting-room"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

The overlay update is synchronous from the IPC client's point of view: `{"ok":true}` means the overlay file was read, validated, and copied into the active runtime configuration. `{"ok":false,...}` means nothing was applied and the previous runtime configuration remains active. Values that affect frame compositing, pause media, camera pause state, segmentation thresholds, smoothing, morphology, and benchmark collection are picked up by the running pipeline on subsequent frames. Startup-only settings still require a service restart.

Invalid JSON, unknown commands, unknown keys, invalid value types, invalid ranges, disabled benchmark reads, and attempts to set `benchmark` return `{"ok":false,...}`. There is intentionally no shutdown command.

## Notes

The current production direction is V4L2 camera input, TensorRT masking, and DRM/KMS fullscreen output. Later systemd service integration should start this binary automatically after boot and open the DRM fullscreen output directly.
