# JONImageProcessor

JONImageProcessor is a C++ base project for a Jetson Orin Nano image processor. The application is intended to read a camera image, process it, and display the processed output fullscreen over HDMI or DisplayPort.

This initial version provides the project foundation: CMake, OpenCV, command-line options, file or camera input, a simple dummy mask as a visible overlay, and output to either an OpenCV window or an MP4 file.

## Table of Contents

- [Prerequisites](#prerequisites)
  - [x86_64 Build Host](#x86_64-build-host)
  - [Jetson Target](#jetson-target)
  - [Jetson Sysroot](#jetson-sysroot)
  - [Optional Jetson Inference](#optional-jetson-inference)
- [Build](#build)
  - [Native Linux Build](#native-linux-build)
  - [Cross-Compile Container Build](#cross-compile-container-build)
  - [Cross-Compile With Jetson Inference](#cross-compile-with-jetson-inference)
- [Install and Run on Jetson](#install-and-run-on-jetson)
  - [Binary Deployment](#binary-deployment)
  - [Runtime Container Status](#runtime-container-status)
- [Example Commands](#example-commands)
- [Command-Line Options](#command-line-options)
- [Camera Configuration](#camera-configuration)
- [Capture Backends](#capture-backends)
- [Low-Latency Camera Mode](#low-latency-camera-mode)
- [Mask Backends](#mask-backends)
- [Display Modes](#display-modes)
- [Display Backends](#display-backends)
- [Verbose Logging](#verbose-logging)
- [Performance Analysis](#performance-analysis)
- [Jetson Orin Nano Notes](#jetson-orin-nano-notes)
- [Planned Service Operation](#planned-service-operation)
- [Planned runtime control](#planned-runtime-control)
- [Test Data](#test-data)

## Prerequisites

### x86_64 Build Host

The preferred reproducible build path is cross-compilation from an x86_64 Linux VM using NVIDIA's JetPack cross-compile container.

Required host software:

- Linux x86_64 host or VM
- Git
- Docker
- Access to `nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:<JetPack version>`
- A Jetson target sysroot copied from the Jetson Orin Nano

Install Docker on a Debian/Ubuntu build host:

```bash
sudo apt update
```

```bash
sudo apt install -y docker.io
```

```bash
sudo systemctl enable --now docker
```

```bash
sudo usermod -aG docker "$USER"
```

Log out and back in, or run:

```bash
newgrp docker
```

Verify Docker access:

```bash
docker run --rm hello-world
```

Pull the JetPack 6.1 cross-compile container:

```bash
docker pull nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

If the Jetson target uses a different JetPack/L4T release, use the matching NGC container tag instead of `6.1`.

### Jetson Target

Required target setup:

- NVIDIA Jetson Orin Nano
- JetPack installed
- Camera available as `/dev/video0`
- HDMI/DisplayPort display for window/fullscreen output
- OpenCV runtime libraries
- V4L2 device access for the runtime user

Check the target JetPack/L4T version on the Jetson:

```bash
cat /etc/nv_tegra_release
```

```bash
dpkg-query -W nvidia-l4t-core
```

For JetPack 6.1, the expected Jetson Linux/L4T line is in the 36.4 family.

Check the camera:

```bash
v4l2-ctl --list-formats-ext -d /dev/video0
```

If the runtime user cannot access the camera, add it to the `video` group on the Jetson:

```bash
sudo usermod -aG video "$USER"
```

Then log out and back in.

### Jetson Sysroot

The cross-build needs target headers, libraries, and CMake package files. Copy these from the Jetson to the x86_64 build host:

```bash
mkdir -p "$HOME/sysroots/orin-nano"
```

```bash
rsync -aHAX --numeric-ids --delete tseiman@jon:/lib/ "$HOME/sysroots/orin-nano/lib/"
```

```bash
rsync -aHAX --numeric-ids --delete --exclude=/libexec/sssd/ tseiman@jon:/usr/ "$HOME/sysroots/orin-nano/usr/"
```

```bash
rsync -aHAX --numeric-ids --delete --exclude=/containerd/ tseiman@jon:/opt/ "$HOME/sysroots/orin-nano/opt/"
```

The resulting sysroot should look like:

```text
$HOME/sysroots/orin-nano/
  lib/
  usr/
  opt/
```

Verify required files:

```bash
test -d "$HOME/sysroots/orin-nano/usr/include" && echo ok
```

```bash
test -d "$HOME/sysroots/orin-nano/usr/lib/aarch64-linux-gnu" && echo ok
```

```bash
find "$HOME/sysroots/orin-nano/usr" -name OpenCVConfig.cmake -o -name opencv4.pc
```

Optional CUDA/TensorRT checks:

```bash
find "$HOME/sysroots/orin-nano/usr" -name cuda_runtime_api.h
```

```bash
find "$HOME/sysroots/orin-nano/usr" "$HOME/sysroots/orin-nano/opt" -name NvInfer.h
```

### Optional Jetson Inference

The default cross-build does not require `jetson-inference`.

Only `--mask-backend jetson` needs:

- `include/jetson-inference/segNet.h`
- `libjetson-inference`
- `libjetson-utils`
- CUDA runtime
- TensorRT

These dependencies must be present in one of these places:

- the mounted Jetson sysroot
- the NVIDIA cross-compile container
- a separate AArch64 install prefix passed as `JETSON_INFERENCE_ROOT`

If `ENABLE_JETSON_INFERENCE=ON` is requested but these files are missing, the build script exits with a clear error before CMake configuration.

## Build

### Native Linux Build

Native build requirements:

- CMake
- C++17 compiler
- OpenCV with CMake package files

```bash
cmake -B build -S .
cmake --build build
```

The executable is created at:

```bash
./build/JONImageProcessor
```

### Cross-Compile Container Build

The cross-compile path uses:

- `scripts/build-jetson-cross.sh`
- `cmake/toolchains/jetson-aarch64.cmake`
- NVIDIA NGC image `nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:<JetPack version>`
- a mounted Jetson sysroot

Default cross-build without `jetson-inference`:

```bash
JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

Expected output:

```text
build-jetson-cross/JONImageProcessor
```

Verify the binary architecture:

```bash
file build-jetson-cross/JONImageProcessor
```

Expected result:

```text
ELF 64-bit LSB pie executable, ARM aarch64, version 1 (GNU/Linux)
```

Use a different JetPack container tag when needed:

```bash
JETPACK_VERSION=6.1 JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

The script also accepts an explicit container image:

```bash
CONTAINER_IMAGE=nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1 JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

### Cross-Compile With Jetson Inference

Build with `jetson-inference` only after the default cross-build works:

```bash
ENABLE_JETSON_INFERENCE=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

If `jetson-inference` is installed into a separate AArch64 prefix:

```bash
ENABLE_JETSON_INFERENCE=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" JETSON_INFERENCE_ROOT=/path/to/aarch64/jetson-inference-prefix ./scripts/build-jetson-cross.sh
```

Expected missing-dependency error when the prefix/sysroot does not contain the required files:

```text
[ERROR] ENABLE_JETSON_INFERENCE=ON, but jetson-inference dependencies were not found.
[ERROR] Required: include/jetson-inference/segNet.h, libjetson-inference, libjetson-utils.
```

More detail is available in [docs/cross-compile-jetson.md](docs/cross-compile-jetson.md).

## Install and Run on Jetson

### Binary Deployment

The current supported deployment path is copying the AArch64 binary to the Jetson and running it directly on the Jetson OS.

Copy the binary:

```bash
scp build-jetson-cross/JONImageProcessor tseiman@jon:~/JONImageProcessor
```

Run on the Jetson:

```bash
ssh tseiman@jon 'chmod +x ~/JONImageProcessor && ~/JONImageProcessor --version'
```

Run with the Logitech BRIO through V4L2:

```bash
ssh -t tseiman@jon '~/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend dummy --background-overlay-color 0,0,255 --background-overlay-alpha 0.35 --benchmark'
```

Run with Jetson segmentation when the binary was built with `ENABLE_JETSON_INFERENCE=ON` and the target has the matching runtime libraries:

```bash
ssh -t tseiman@jon '~/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend jetson --background-overlay-color 0,0,255 --background-overlay-alpha 0.35 --benchmark'
```

If dynamic libraries are missing on the Jetson, inspect them with:

```bash
ssh tseiman@jon 'ldd ~/JONImageProcessor'
```

### Runtime Container Status

This repository currently does not provide a Jetson runtime Dockerfile or a packaged runtime image. The container currently documented and automated here is the NVIDIA x86_64 cross-compile container, not an application runtime container for the Jetson.

Running JONImageProcessor inside a Jetson runtime container will require a separate runtime image based on a compatible JetPack/L4T base image, plus camera/display device mounts such as `/dev/video0`, `/tmp/.X11-unix`, and the relevant NVIDIA runtime configuration. That packaging step is intentionally not implemented in this project yet.

## Example Commands

```bash
./build/JONImageProcessor --help
```

```bash
./build/JONImageProcessor --version
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --output window
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --output file --output-file output.mp4
```

```bash
./build/JONImageProcessor --device /dev/video0 --width 1280 --height 720 --mask-width 256 --mask-height 144
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --display-mode fit
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --display-mode fill
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --display-mode stretch
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --fullscreen --display-mode fit --output-width 1920 --output-height 1080 --verbose
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 -v
```

```bash
./build/JONImageProcessor --input testdata/Test1_pixabay_Video_4k.mp4 --benchmark --max-frames 500
```

```bash
./build/JONImageProcessor --input testdata/Test1_pixabay_Video_4k.mp4 --benchmark --no-display --no-mask --no-overlay
```

## Command-Line Options

The help output is generated from the same central option table that is used for `getopt_long`:

```bash
./build/JONImageProcessor --help
```

Important options:

- `--input <path>` reads video from a file.
- `--device <path>` reads from a camera device. The default is `/dev/video0`.
- `--output window` shows an OpenCV window.
- `--output file` writes an MP4 file.
- `--fullscreen` switches the window to fullscreen when `--output window` is used.
- `--display-mode <mode>` controls how the processed image is scaled into the current window or fullscreen area.
- `--display-backend <backend>` selects the display backend. The current default and only supported backend is `highgui`.
- `--capture-backend <backend>` selects the camera capture backend. Supported values are `opencv` and `v4l2`; the default is `opencv`. File input always uses OpenCV.
- `--output-width <pixels>` and `--output-height <pixels>` explicitly define the display render surface. They must be specified together.
- `--width` and `--height` configure processing dimensions and requested camera capture size.
- `--mask-backend <backend>` selects the mask backend. Supported values are `none`, `dummy`, and `jetson`; the default is `dummy`.
- `--segmentation-width <pixels>` and `--segmentation-height <pixels>` configure segmentation inference size. Legacy `--mask-width` and `--mask-height` map to the same setting.
- `--background-overlay-color <R,G,B>` sets the background debug overlay color. The default is `0,0,255`.
- `--background-overlay-alpha <0.0..1.0>` sets the background debug overlay opacity. The default is `0.35`.
- `--camera-format <format>` requests a camera pixel format. Supported values are `MJPG` and `YUYV`; the default is `MJPG`.
- `--camera-fps <fps>` requests a camera frame rate. The default is `30`.
- `--low-latency` enables low-latency live camera capture. Camera input enables this mode automatically; file input keeps sequential frame processing.
- `--version` prints the 7-character Git commit hash captured at CMake configure time. A semantic release version, such as `0.1.0`, is only printed when the build is configured exactly on a Git release tag like `v0.1.0` or `0.1.0`.
- `--benchmark` enables benchmark mode.
- `--max-frames <n>` stops automatically after n processed frames.
- `--no-display` disables window and file output.
- `--no-mask` disables mask generation and mask upscaling.
- `--no-overlay` disables overlay rendering.

In window mode, `ESC` or `q` exits the program cleanly.

## Camera Configuration

When using `--device`, the application requests the configured camera pixel format, frame size, and frame rate through the selected camera capture backend. `--width` and `--height` are used both as processing size and requested camera capture size.

```bash
./build/JONImageProcessor --device /dev/video0 --width 1920 --height 1080 --camera-format MJPG --camera-fps 30
```

```bash
./build/JONImageProcessor --device /dev/video0 --width 1280 --height 720 --camera-format MJPG --camera-fps 60
```

Many USB webcams need MJPG for high resolutions and useful frame rates. Uncompressed YUYV often supports only very low frame rates at 1080p or above.

Verbose mode logs the requested camera settings and the active settings reported by the capture backend after configuration. If the camera does not accept the requested format, size, or FPS, a warning is emitted.

## Capture Backends

The capture path is separated from the video processing pipeline through an `ICaptureBackend` interface. File input always uses OpenCV. Camera input can use either OpenCV or direct V4L2 on Linux.

Supported backends:

- `opencv`: portable OpenCV capture. This is the default and is useful for files, first tests, and non-Linux development systems.
- `v4l2`: Linux camera backend using direct V4L2 access with mmap streaming. This is intended for low-latency live camera use on Jetson and other Linux systems.

Select OpenCV camera capture explicitly:

```bash
./build/JONImageProcessor --device /dev/video0 --capture-backend opencv --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --benchmark --max-frames 100 --no-display --no-mask --no-overlay
```

Select V4L2 camera capture on Linux or Jetson:

```bash
./build/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --benchmark --max-frames 100 --no-display --no-mask --no-overlay
```

The V4L2 backend currently supports `MJPG` and `YUYV`. MJPG frames are decoded with OpenCV `imdecode`; YUYV frames are converted to BGR with OpenCV color conversion. Automatic V4L2 format enumeration and automatic format selection are intentionally not implemented yet.

## Low-Latency Camera Mode

Live camera input prioritizes low latency over processing every frame. Camera input automatically enables low-latency mode; file input keeps sequential frame processing.

In low-latency mode a capture thread continuously reads from the camera and stores only the newest frame. The processing thread always consumes the latest available frame. Older frames may be overwritten when processing is slower than camera capture, which keeps visible latency low.

```bash
./build/JONImageProcessor --device /dev/video0 --width 1920 --height 1080 --camera-format MJPG --camera-fps 30 --low-latency
```

The application also requests an OpenCV camera buffer size of `1`. Some camera backends may ignore this setting; the application logs the request and continues.

## Mask Backends

Mask generation is separated from the video pipeline through an `IMaskBackend` interface. A generated mask is an OpenCV `CV_8UC1` image where `255` means person and `0` means background.

Supported backends:

- `none`: no mask is generated.
- `dummy`: keeps the previous moving-circle debug mask. The circle is treated as the person area.
- `jetson`: uses NVIDIA Jetson `jetson-inference` `segNet` with TensorRT/CUDA acceleration. This backend is only available when the binary is configured with `-DJON_ENABLE_JETSON_INFERENCE=ON` and built against `jetson-inference` and `jetson-utils`.

The current debug visualization keeps the detected person unchanged and applies a semi-transparent color overlay to the background.

Dummy mask example:

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --mask-backend dummy --background-overlay-color 0,0,255 --background-overlay-alpha 0.35
```

Disable mask generation:

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --mask-backend none
```

Jetson segmentation example:

```bash
./build/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend jetson --background-overlay-color 0,0,255 --background-overlay-alpha 0.35 --benchmark
```

Cross-build with Jetson inference support after the default cross-build works:

```bash
ENABLE_JETSON_INFERENCE=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

If `jetson-inference` is installed into a separate AArch64 prefix, pass that prefix explicitly:

```bash
ENABLE_JETSON_INFERENCE=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" JETSON_INFERENCE_ROOT=/path/to/aarch64/jetson-inference-prefix ./scripts/build-jetson-cross.sh
```

The Jetson backend currently uses the `fcn-resnet18-voc-320x320` `segNet` model because it provides a `person` class. The model output is converted into a binary person/background mask. The first run may take longer while TensorRT builds or loads its optimized engine.

## Display Modes

The default display mode is `fit`.

- `fit` preserves the aspect ratio, keeps the complete image visible, centers it horizontally and vertically, and uses black bars when the display area has a different aspect ratio.
- `fill` preserves the aspect ratio, fills the complete display area, keeps the image centered, and crops overflowing image areas when needed.
- `stretch` ignores the aspect ratio and scales the image exactly to the display area. This avoids bars and cropping but may distort the image.

For window output, the visible processing frame keeps the original input aspect ratio and fits inside the configured `--width` x `--height` processing bounds. This prevents a square `--width` x `--height` configuration from turning a 16:9 camera image into a square display image. File output still writes exactly `--width` x `--height`.

The application uses the current OpenCV window image area for display sizing when that value is reliable. In fullscreen mode, if the primary screen size is available, that screen size is used as the render canvas because OpenCV HighGUI can report stale or invalid window sizes on some platforms. If no reliable screen or window size is available, the renderer falls back to the configured display surface. Without an explicit display surface, that fallback is the processing size configured by `--width` and `--height`.

Use `--output-width` and `--output-height` to define the display render surface explicitly. This is useful on platforms where OpenCV HighGUI does not report the real fullscreen size reliably, especially on macOS. Fullscreen is still requested, but the render calculation can use the explicit size instead of depending on an unreliable window rectangle.

In verbose mode, display diagnostics include input frame size, processing size, window rectangle size, canvas size, display mode, and destination rectangle.

## Display Backends

The display path is separated from the video processing pipeline through an `IDisplayBackend` interface. This keeps decoding, resizing, mask generation, and overlay rendering independent from the final output system.

Currently supported backend:

- `highgui`: OpenCV HighGUI window output. This preserves the current behavior and remains the default.

The backend can be selected explicitly:

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --display-backend highgui
```

Planned future backends include SDL2, GStreamer, DRM/KMS, and Jetson-specific output paths. Adding one of those should not require changes to the video processing, mask, overlay, camera input, or file input logic.

## Verbose Logging

The application uses a small central logging layer with prefixed output:

- `[INFO]` for normal lifecycle messages
- `[WARNING]` for recoverable fallback behavior
- `[ERROR]` for failures
- `[VERBOSE]` for diagnostics that are only shown when `--verbose` or `-v` is enabled

Verbose logging is intentionally simple and currently writes directly to standard output. It is designed so it can later be replaced by syslog, journald, or a dedicated logging system when JONImageProcessor runs as a systemd service.

Examples:

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 --verbose
```

```bash
./build/JONImageProcessor --input testdata/Test2_pixabay_Video_HD.mp4 -v
```

Verbose startup diagnostics include the release version state, Git version, build date, operating system, OpenCV version, input source, output mode, display mode, processing size, mask size, and fullscreen state.

Verbose display diagnostics include display input frame size, detected primary screen size, backend window size, output canvas size, display mode, and destination rectangle. This makes platform-specific backend behavior visible, especially when fullscreen window sizing differs between Linux and macOS. Performance diagnostics are emitted about once per second and include current FPS, average FPS, and processed frame count.

## Performance Analysis

Benchmark mode measures where frame time is spent without changing the processing pipeline by default:

```bash
./build/JONImageProcessor --input testdata/Test1_pixabay_Video_4k.mp4 --benchmark --max-frames 500
```

Use `--max-frames <n>` to make benchmark runs repeatable and automatically stop after a fixed number of frames.

Use `--no-display` to skip window creation and file output. This isolates decode, resize, mask, and overlay cost:

```bash
./build/JONImageProcessor --input testdata/Test1_pixabay_Video_4k.mp4 --benchmark --no-display --max-frames 500
```

Use `--no-mask` to skip mask generation and mask upscaling:

```bash
./build/JONImageProcessor --input testdata/Test1_pixabay_Video_4k.mp4 --benchmark --no-display --no-mask --max-frames 500
```

Use `--no-overlay` to skip overlay rendering:

```bash
./build/JONImageProcessor --input testdata/Test1_pixabay_Video_4k.mp4 --benchmark --no-display --no-mask --no-overlay --max-frames 500
```

Benchmark output reports average time for capture wait, frame handover, decode, resize, segmentation preprocess, segmentation inference, segmentation postprocess, mask upscale, overlay, display, processing total, pipeline total, and effective FPS. The percentage distribution separates `Capture wait`, `Frame handover`, segmentation stages, and `Unclassified other` so live-camera runs show whether time is spent waiting for camera frames, running inference, copying the latest frame into the processing loop, or actually processing the frame.

`Processing total` measures the work after a frame is available. `Pipeline total` includes frame acquisition wait/handover plus processing. This distinction is important for live camera measurements, where a 30 FPS camera naturally limits the pipeline rate even if processing is much faster.

In low-latency camera mode, benchmark output also reports captured frames, processed frames, dropped/overwritten frames, capture FPS, and processing FPS.

The goal is to compare macOS development systems, Linux VMs, and Jetson Orin Nano runs objectively before deciding where optimization work should happen.

## Jetson Orin Nano Notes

This version intentionally uses only CMake, C++17, and OpenCV so it can build on a normal Linux VM and later on the Jetson Orin Nano.

On the Jetson, OpenCV and camera access should be verified separately before service operation. `/dev/video0` is the default for USB cameras. The `v4l2` capture backend is preferred for low-latency USB camera input.

For `--mask-backend jetson`, the AArch64 build environment must provide JetPack-compatible CUDA/TensorRT runtime files and `jetson-inference`/`jetson-utils` headers and libraries. Do not infer from missing `/usr/local/include` files on the Jetson that a native Jetson source install is required. The preferred reproducible path is cross-compilation from an x86_64 Linux VM with a matching Jetson sysroot and, if needed, a separate AArch64 `jetson-inference` prefix. See `docs/cross-compile-jetson.md` for details.

## Planned Service Operation

Future versions should run automatically as a Linux service/daemon through systemd and display the processed camera image fullscreen on HDMI or DisplayPort.

This initial step does not include a systemd unit, daemon mode, or display-specific initialization.

Long-term appliance flow:

```text
Jetson boots
-> systemd starts JONImageProcessor
-> HDMI resolution is detected
-> fullscreen output is opened
-> the image is centered correctly
-> the display mode controls scaling
```

## Planned runtime control

Processing settings already live in a central `ProcessorConfig` structure. This structure is intentionally prepared so it can be updated at runtime later.

The planned approach is a local Unix domain socket that accepts JSON commands. Future commands should allow settings such as background image, blur strength, transparency, fullscreen, and mask debug overlay to be changed at runtime.

Not implemented yet:

- no Unix domain socket control
- no WebAPI
- no JSON runtime protocol

## Test Data

The `testdata/` directory is intended for local test videos and is not ignored globally.
