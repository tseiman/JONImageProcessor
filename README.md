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
- [Running As Daemon](#running-as-daemon)
- [Running As Systemd Service](#running-as-systemd-service)
- [Command Line Options](#command-line-options)
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
./JONImageProcessor --no-daemon --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --mask-threshold 0.7 --mask-smoothing 0.65 --mask-morphology light --background-effect blur --blur-strength 85 --display-backend drm --fullscreen --benchmark
```

### Color Background

```bash
./JONImageProcessor --no-daemon --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --mask-threshold 0.5 --mask-smoothing 0.65 --mask-morphology light --background-effect color --background-overlay-color 0,255,0 --background-overlay-alpha 1.0 --display-backend drm --fullscreen --benchmark
```

### Image Background

```bash
./JONImageProcessor --no-daemon --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --mask-threshold 0.7 --mask-smoothing 0.65 --mask-morphology light --background-effect image --background-image "$HOME/JONImageProcessor/background.jpg" --display-backend drm --fullscreen --benchmark
```

### HighGUI Window

```bash
./JONImageProcessor --no-daemon --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend highgui
```

### Video File Test

```bash
./JONImageProcessor --no-daemon --input test.mp4 --mask-model "$MODEL_PATH" --background-effect blur --display-backend highgui
```

## Running As Daemon

Without `--no-daemon`, JONImageProcessor starts in daemon mode, detaches from the terminal, redirects standard streams to `/dev/null`, and writes logs through syslog/journald. Use foreground mode for development and benchmarking:

```bash
./JONImageProcessor --no-daemon --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend drm --fullscreen --benchmark
```

Daemon example:

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend drm --fullscreen
```

Stop a daemonized process with `SIGTERM`, for example:

```bash
pkill -TERM JONImageProcessor
```

## Running As Systemd Service

The example unit is in `packaging/systemd/JONImageProcessor.service` and uses daemon mode. It does not pass `--no-daemon`.

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
- `--version`: show release/git version.
- `-n`, `--no-daemon`: run as foreground process instead of daemon mode.
- `-v`, `--verbose`: enable detailed logs.
- `-i`, `--input <path>`: use a video file as input. Without this option, the V4L2 camera is used.
- `-d`, `--device <path>`: V4L2 camera device. Default: `/dev/video0`.
- `-p`, `--processing-size <WxH>`: processing size and requested camera size. Default: `1920x1080`.
- `-o`, `--output-size <WxH>`: explicit render canvas size. Default: `auto`.
- `--camera-format <MJPG|YUYV>`: requested V4L2 pixel format. Default: `MJPG`.
- `--mask-model <path>`: TensorRT mask model path. Required unless `--no-mask` is used.
- `-s`, `--segmentation-size <WxH>`: TensorRT input size. Default: `384x384`.
- `--mask-threshold <0.0..1.0>`: foreground threshold. Default: `0.5`.
- `--mask-smoothing <0.0..1.0>`: temporal mask smoothing. Default: `0.65`.
- `--mask-morphology <off|light|strong>`: mask cleanup mode. Default: `light`.
- `--background-effect <color|blur|image>`: background effect. Default: `color`.
- `--background-image <path>`: JPEG or PNG image used by `--background-effect image`.
- `--background-overlay-color <R,G,B>`: color used by `--background-effect color`. Ignored for blur/image. Default: `0,255,0`.
- `--background-overlay-alpha <0.0..1.0>`: alpha used by `--background-effect color`. Ignored for blur/image. Default: `0.35`.
- `--blur-strength <1..100>`: blur strength used by `--background-effect blur`. Default: `15`.
- `--display-backend <highgui|drm>`: display backend. Default: `highgui`.
- `--fullscreen`: request fullscreen display output.
- `--benchmark`: print benchmark statistics.
- `--no-display`: disable display output.
- `--no-mask`: disable TensorRT mask generation.
- `--no-overlay`: disable background effect rendering.
- `--ipc-socket <path>`: Unix domain socket path for runtime control. Use `none` to disable IPC. Default: `/tmp/jonimageprocessor.sock`.

## Runtime Behavior

Camera input always uses V4L2 and low-latency capture. The capture thread keeps only the newest frame, so old frames are overwritten instead of queued.

Video file input uses OpenCV file capture and processes frames sequentially.

Display mode is fixed to fill. The image fills the output canvas while preserving aspect ratio. Cropping is allowed; stretching is not used.

## Benchmarking

Use `--benchmark` to print timing statistics for capture, resize, TensorRT preprocessing, TensorRT inference, postprocessing, mask upscale, background effect, display, and total frame time.

For pipeline timing without display or effects:

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --no-display --no-mask --no-overlay --benchmark
```

Stop long-running camera benchmarks with `Ctrl-C`. SIGINT is handled and prints the final benchmark summary.

## IPC Control Interface

JONImageProcessor exposes a small Unix domain socket control interface for runtime parameters. The default socket is `/tmp/jonimageprocessor.sock`; use `--ipc-socket <path>` to change it or `--ipc-socket none` to disable IPC. The protocol is NDJSON: send one JSON request line and receive one JSON response line.

Commands:

- `get`: read one key.
- `set`: update one writable key.
- `list`: read all runtime keys.

Writable keys:

- `mask_threshold`: float `0.0..1.0`
- `mask_smoothing`: float `0.0..1.0`
- `mask_morphology`: `off`, `light`, `strong`
- `background_effect`: `color`, `blur`, `image`
- `background_image`: string path
- `background_overlay_color`: `R,G,B`
- `background_overlay_alpha`: float `0.0..1.0`
- `blur_strength`: integer `1..100`
- `no_mask`: boolean
- `no_overlay`: boolean

Read-only key:

- `benchmark`: current benchmark snapshot

Examples:

```bash
echo '{"cmd":"get","key":"mask_threshold"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"set","key":"mask_threshold","value":0.6}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"list"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

```bash
echo '{"cmd":"get","key":"benchmark"}' | socat - UNIX-CONNECT:/tmp/jonimageprocessor.sock
```

Invalid JSON, unknown commands, unknown keys, invalid value types, invalid ranges, and attempts to set `benchmark` return `{"ok":false,...}`. There is intentionally no shutdown command.

## Notes

The current production direction is V4L2 camera input, TensorRT masking, and DRM/KMS fullscreen output. Later systemd service integration should start this binary automatically after boot and open the DRM fullscreen output directly.
