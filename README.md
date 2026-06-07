# JONImageProcessor

JONImageProcessor is a C++17 image-processing base application for NVIDIA Jetson Orin Nano. It reads video from a file or camera, processes the image, renders a debug mask/overlay, and displays the result through an OpenCV HighGUI window or writes it to MP4.

The intended deployment is a Jetson appliance: the Jetson boots, the application starts automatically later through systemd, reads the camera, and renders fullscreen to HDMI or DisplayPort. This repository currently focuses on the application binary and reproducible cross-builds.

## Table of Contents

- [Prerequisites](#prerequisites)
  - [Build Host](#build-host)
  - [Jetson Target](#jetson-target)
  - [Jetson Packages](#jetson-packages)
  - [NVIDIA Cross-Compile Container](#nvidia-cross-compile-container)
- [Prepare the Sysroot](#prepare-the-sysroot)
- [Build](#build)
  - [Cross-Build Without Jetson Inference](#cross-build-without-jetson-inference)
  - [Prepare jetson-inference Prefix](#prepare-jetson-inference-prefix)
  - [Cross-Build With Jetson Inference](#cross-build-with-jetson-inference)
- [Install and Run on Jetson](#install-and-run-on-jetson)
  - [Install the Binary](#install-the-binary)
  - [Install jetson-inference Runtime Libraries](#install-jetson-inference-runtime-libraries)
  - [Run the Application](#run-the-application)
- [Useful Runtime Commands](#useful-runtime-commands)
- [Command-Line Options](#command-line-options)
- [Camera Configuration](#camera-configuration)
- [Capture Backends](#capture-backends)
- [Mask Backends](#mask-backends)
- [Display Modes](#display-modes)
- [Verbose Logging](#verbose-logging)
- [Performance Analysis](#performance-analysis)
- [Planned Service Operation](#planned-service-operation)
- [Planned Runtime Control](#planned-runtime-control)

## Prerequisites

### Build Host

Use an x86_64 Linux VM or workstation as the build host.

Install the required host tools:

```bash
sudo apt update && sudo apt install -y git docker.io rsync openssh-client file wget ca-certificates dialog
```

Enable Docker and allow your user to run Docker:

```bash
sudo systemctl enable --now docker && sudo usermod -aG docker "$USER"
```

Log out and back in, then verify Docker:

```bash
docker run --rm hello-world
```

Clone this repository on the build host:

```bash
git clone git@github.com:tseiman/JONImageProcessor.git && cd JONImageProcessor
```

### Jetson Target

This README assumes:

- NVIDIA Jetson Orin Nano
- JetPack 6.1 / L4T 36.4 family
- Camera available as `/dev/video0`
- SSH access from the build host, for example `tseiman@jon`
- Sysroot location on the build host: `$HOME/sysroots/orin-nano`
- Optional jetson-inference prefix on the build host: `$HOME/aarch64-prefixes/jetson-inference`

Check the Jetson version on the Jetson:

```bash
cat /etc/nv_tegra_release && dpkg-query -W nvidia-l4t-core
```

Check the camera on the Jetson:

```bash
v4l2-ctl --list-formats-ext -d /dev/video0
```

Allow the runtime user to access V4L2 devices:

```bash
sudo usermod -aG video "$USER"
```

Log out and back in after changing group membership.

### Jetson Packages

Install the target packages on the Jetson before syncing the sysroot:

```bash
sudo apt update && sudo apt install -y rsync openssh-server v4l-utils libopencv-dev libglew-dev libglu1-mesa-dev libgstrtspserver-1.0-dev libjson-glib-dev libsoup2.4-dev
```

JetPack provides CUDA and TensorRT. The package list above adds the development files that were needed for this cross-build path and for the `jetson-inference` prefix.

### NVIDIA Cross-Compile Container

The build uses NVIDIA's NGC cross-compile container:

```text
nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

Pull it on the x86_64 build host:

```bash
docker pull nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

This is a build container, not the JONImageProcessor runtime container. The application is deployed directly onto the Jetson OS in this project stage.

## Prepare the Sysroot

Create the sysroot directory on the build host:

```bash
mkdir -p "$HOME/sysroots/orin-nano"
```

Copy `/usr`, `/lib`, and `/opt` from the Jetson:

```bash
rsync -aHAX --numeric-ids --exclude='/libexec/sssd/' tseiman@jon:/usr/ "$HOME/sysroots/orin-nano/usr/"
```

```bash
rsync -aHAX --numeric-ids tseiman@jon:/lib/ "$HOME/sysroots/orin-nano/lib/"
```

```bash
rsync -aHAX --numeric-ids --exclude='/containerd/' tseiman@jon:/opt/ "$HOME/sysroots/orin-nano/opt/"
```

If rsync reports permission errors for `/usr/libexec/sssd/*`, those files are not needed for this build. If `/lib` conflicts with an existing symlink or directory, keep the already synced `usr/lib/aarch64-linux-gnu` content and verify the required libraries below before changing the sysroot layout.

Verify the important files:

```bash
test -d "$HOME/sysroots/orin-nano/usr/include" && test -d "$HOME/sysroots/orin-nano/usr/lib/aarch64-linux-gnu" && echo ok
```

```bash
find "$HOME/sysroots/orin-nano/usr" -name OpenCVConfig.cmake -o -name opencv4.pc
```

```bash
find "$HOME/sysroots/orin-nano/usr" -name cuda_runtime_api.h
```

```bash
find "$HOME/sysroots/orin-nano/usr" "$HOME/sysroots/orin-nano/opt" -name NvInfer.h
```

```bash
find "$HOME/sysroots/orin-nano" -name 'libcublas.so' -o -name 'libnppicc.so' -o -name 'libcudla.so' -o -name 'libnvdla_compiler.so' -o -name 'libgstrtspserver-1.0.so' -o -name 'libjson-glib-1.0.so' -o -name 'libsoup-2.4.so'
```

## Build

### Cross-Build Without Jetson Inference

This is the baseline build. It supports OpenCV/V4L2 capture and the `none`/`dummy` mask backends.

```bash
JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

Verify the output:

```bash
file build-jetson-cross/JONImageProcessor
```

Expected architecture:

```text
ELF 64-bit LSB pie executable, ARM aarch64, GNU/Linux
```

### Prepare jetson-inference Prefix

Only do this if you need `--mask-backend jetson`.

Clone `jetson-inference` on the build host:

```bash
mkdir -p "$HOME/src" "$HOME/aarch64-prefixes" && git clone --recursive https://github.com/dusty-nv/jetson-inference.git "$HOME/src/jetson-inference"
```

Start the NVIDIA cross-compile container:

```bash
docker run --rm -it -v "$HOME/src/jetson-inference:/workspace/jetson-inference" -v "$HOME/sysroots/orin-nano:/workspace/sysroot:ro" -v "$HOME/aarch64-prefixes/jetson-inference:/workspace/install" -v "$PWD/cmake/toolchains/jetson-aarch64.cmake:/workspace/jetson-aarch64.cmake:ro" -w /workspace/jetson-inference nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1 /bin/bash
```

Inside the container, patch `jetson-inference` so its legacy `FindCUDA.cmake` links AArch64 CUDA libraries from the mounted sysroot instead of x86_64 host CUDA:

```bash
cp -n utils/cuda/FindCUDA.cmake utils/cuda/FindCUDA.cmake.codex-backup
```

```bash
python3 - <<'PY'
from pathlib import Path
p = Path("utils/cuda/FindCUDA.cmake")
s = p.read_text()
old = '''  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    if( EXISTS "/usr/local/cuda/lib64/lib${_names}.so" )
        set(${_var} "/usr/local/cuda/lib64/lib${_names}.so")
    endif()
  else()'''
new = '''  if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    if( EXISTS "/workspace/sysroot/usr/local/cuda-12.6/targets/aarch64-linux/lib/lib${_names}.so" )
        set(${_var} "/workspace/sysroot/usr/local/cuda-12.6/targets/aarch64-linux/lib/lib${_names}.so")
    elseif( EXISTS "/usr/local/cuda/lib64/lib${_names}.so" )
        set(${_var} "/usr/local/cuda/lib64/lib${_names}.so")
    endif()
  else()'''
if old not in s:
    raise SystemExit("expected FindCUDA aarch64 block not found")
p.write_text(s.replace(old, new))
PY
```

Configure and build `jetson-inference` inside the container:

```bash
unset PKG_CONFIG_PATH && export PKG_CONFIG_SYSROOT_DIR=/workspace/sysroot && export PKG_CONFIG_LIBDIR=/workspace/sysroot/usr/lib/aarch64-linux-gnu/pkgconfig:/workspace/sysroot/usr/lib/pkgconfig:/workspace/sysroot/usr/share/pkgconfig:/workspace/sysroot/lib/aarch64-linux-gnu/pkgconfig && cmake -B build-aarch64 -S . -DCMAKE_TOOLCHAIN_FILE=/workspace/jetson-aarch64.cmake -DCMAKE_INSTALL_PREFIX=/workspace/install -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="/workspace/sysroot/usr;/workspace/sysroot/usr/local;/workspace/sysroot/usr/local/cuda-12.6/targets/aarch64-linux" -DCUDA_TOOLKIT_ROOT_DIR=/usr/local/cuda -DCUDA_TOOLKIT_TARGET_DIR=/workspace/sysroot/usr/local/cuda-12.6/targets/aarch64-linux -DCUDA_HOST_COMPILER=/usr/bin/aarch64-linux-gnu-g++ -DCMAKE_CXX_FLAGS="-I/workspace/sysroot/usr/include/gstreamer-1.0 -I/workspace/sysroot/usr/include/glib-2.0 -I/workspace/sysroot/usr/lib/aarch64-linux-gnu/glib-2.0/include -I/workspace/sysroot/usr/include/aarch64-linux-gnu" -DCMAKE_C_FLAGS="-I/workspace/sysroot/usr/include/gstreamer-1.0 -I/workspace/sysroot/usr/include/glib-2.0 -I/workspace/sysroot/usr/lib/aarch64-linux-gnu/glib-2.0/include -I/workspace/sysroot/usr/include/aarch64-linux-gnu"
```

```bash
cmake --build build-aarch64 -- -j$(nproc)
```

```bash
cmake --install build-aarch64
```

Leave the container and verify the prefix on the build host:

```bash
test -f "$HOME/aarch64-prefixes/jetson-inference/include/jetson-inference/segNet.h" && test -f "$HOME/aarch64-prefixes/jetson-inference/lib/libjetson-inference.so" && test -f "$HOME/aarch64-prefixes/jetson-inference/lib/libjetson-utils.so" && echo ok
```

```bash
file "$HOME/aarch64-prefixes/jetson-inference/lib/libjetson-inference.so" "$HOME/aarch64-prefixes/jetson-inference/lib/libjetson-utils.so"
```

Download the segmentation model data on the build host. This step does not install anything on the Jetson yet; it only downloads the model files into the local `jetson-inference` checkout:

```bash
cd "$HOME/src/jetson-inference/tools" && ./download-models.sh
```

In the model selection dialog, keep `FCN-ResNet18-Pascal-VOC-320x320` selected. JONImageProcessor currently uses this model for `--mask-backend jetson`.

If the tool exits with `Model selection status: 127`, install the missing host-side dialog dependencies and run it again:

```bash
sudo apt update && sudo apt install -y dialog wget ca-certificates
```

Verify that the model directory exists:

```bash
test -f "$HOME/src/jetson-inference/data/networks/models.json" && test -f "$HOME/src/jetson-inference/data/networks/FCN-ResNet18-Pascal-VOC-320x320/fcn_resnet18.onnx" && echo ok
```

### Cross-Build With Jetson Inference

Build JONImageProcessor with the `jetson` mask backend enabled:

```bash
ENABLE_JETSON_INFERENCE=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" JETSON_INFERENCE_ROOT="$HOME/aarch64-prefixes/jetson-inference" ./scripts/build-jetson-cross.sh
```

Verify the output:

```bash
file build-jetson-cross/JONImageProcessor
```

## Install and Run on Jetson

### Install the Binary

Execute these commands on the Jetson:

```bash
mkdir -p ~/JONImageProcessor
```

Copy the AArch64 binary from the build host to the Jetson:

```bash
scp build-jetson-cross/JONImageProcessor tseiman@jon:~/JONImageProcessor/JONImageProcessor
```

Execute these commands on the Jetson:

```bash
chmod +x ~/JONImageProcessor/JONImageProcessor && file ~/JONImageProcessor/JONImageProcessor && ~/JONImageProcessor/JONImageProcessor --version
```

### Install jetson-inference Runtime Libraries

Only do this if the binary was built with `ENABLE_JETSON_INFERENCE=ON`.

Copy the AArch64 `jetson-inference` prefix from the build host to the Jetson:

```bash
rsync -aHAX "$HOME/aarch64-prefixes/jetson-inference/" tseiman@jon:~/JONImageProcessor/jetson-inference/
```

Copy the `jetson-inference` model manifest and model data from the build host to the Jetson:

```bash
rsync -aHAX "$HOME/src/jetson-inference/data/networks/" tseiman@jon:~/JONImageProcessor/networks/
```

The model download runs on the build host, but the application loads `networks/models.json` at runtime on the Jetson. The copy command above is required before running `--mask-backend jetson`.

Execute these commands on the Jetson when using the `jetson` mask backend:

```bash
export LD_LIBRARY_PATH="$HOME/JONImageProcessor/jetson-inference/lib:$LD_LIBRARY_PATH"
```

Check missing libraries on the Jetson:

```bash
LD_LIBRARY_PATH="$HOME/JONImageProcessor/jetson-inference/lib:$LD_LIBRARY_PATH" ldd ~/JONImageProcessor/JONImageProcessor | grep "not found" || true
```

### Run the Application

Execute these commands on the Jetson.

Run with the dummy mask backend:

```bash
~/JONImageProcessor/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend dummy --display-mode fit --fullscreen --benchmark
```

Run with Jetson/TensorRT segmentation:

```bash
cd ~/JONImageProcessor && LD_LIBRARY_PATH="$HOME/JONImageProcessor/jetson-inference/lib:$LD_LIBRARY_PATH" ./JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend jetson --background-overlay-color 0,0,255 --background-overlay-alpha 0.35 --display-mode fit --fullscreen --benchmark
```

The first Jetson segmentation run can take longer because TensorRT may build or load an optimized engine.

## Useful Runtime Commands

Execute these commands on the Jetson.

Show help on the Jetson:

```bash
~/JONImageProcessor/JONImageProcessor --help
```

Show version/build information on the Jetson:

```bash
~/JONImageProcessor/JONImageProcessor --version
```

Benchmark camera capture on Jetson:

```bash
~/JONImageProcessor/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --benchmark --max-frames 300 --no-display --no-mask --no-overlay
```

## Command-Line Options

The help output is generated from the same central option table used by `getopt_long`:

```bash
~/JONImageProcessor/JONImageProcessor --help
```

Important options:

- `--input <path>` reads video from a file.
- `--device <path>` reads from a camera device. Default: `/dev/video0`.
- `--capture-backend <backend>` selects `opencv` or `v4l2`.
- `--output <mode>` selects `window` or `file`.
- `--output-file <path>` sets the MP4 output path for file output.
- `--width <pixels>` and `--height <pixels>` set processing size and requested camera size.
- `--camera-format <MJPG|YUYV>` requests the camera pixel format.
- `--camera-fps <fps>` requests the camera frame rate.
- `--mask-backend <none|dummy|jetson>` selects mask generation.
- `--background-overlay-color <R,G,B>` sets background overlay color.
- `--background-overlay-alpha <0.0..1.0>` sets overlay opacity.
- `--display-mode <fit|fill|stretch>` controls scaling into the window/fullscreen canvas.
- `--fullscreen` requests fullscreen window output.
- `--output-width <pixels>` and `--output-height <pixels>` explicitly define the render canvas.
- `--display-backend highgui` selects the current OpenCV display backend.
- `--benchmark` enables timing output.
- `--max-frames <n>` exits after n processed frames.
- `--no-display`, `--no-mask`, and `--no-overlay` disable pipeline stages for diagnostics.
- `--verbose` or `-v` enables detailed diagnostics.
- `--version` prints release and Git/build information.

In window mode, `ESC` or `q` exits cleanly.

## Camera Configuration

For the Logitech BRIO and similar USB webcams, prefer MJPG for higher resolutions and frame rates:

```bash
~/JONImageProcessor/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --width 1280 --height 720 --camera-format MJPG --camera-fps 30
```

Verbose mode logs requested and active camera format, size, and FPS.

## Capture Backends

File input always uses OpenCV. Camera input can use:

- `opencv`: portable OpenCV capture.
- `v4l2`: direct Linux V4L2 capture with mmap streaming, preferred on Jetson for live camera use.

The V4L2 backend supports MJPG and YUYV. Automatic V4L2 format enumeration is not implemented yet.

## Mask Backends

Supported mask backends:

- `none`: no mask.
- `dummy`: moving-circle debug mask.
- `jetson`: TensorRT/CUDA segmentation through `jetson-inference` `segNet`.

The debug visualization keeps the detected person unchanged and tints the background with the configured overlay color and alpha.

## Display Modes

Default: `fit`.

- `fit`: preserve aspect ratio, show the full image, center it, and allow black bars.
- `fill`: preserve aspect ratio, fill the whole canvas, center it, and crop overflow.
- `stretch`: ignore aspect ratio and scale exactly to the canvas.

Use `--output-width` and `--output-height` when OpenCV HighGUI reports unreliable fullscreen dimensions.

## Verbose Logging

Verbose logging is intentionally simple and prints prefixed messages to stdout:

- `[INFO]`
- `[WARNING]`
- `[ERROR]`
- `[VERBOSE]`
- `[BENCH]`

Enable it with:

```bash
~/JONImageProcessor/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --verbose
```

Verbose diagnostics include version/build info, OpenCV version, operating system, camera settings, display canvas/window sizes, destination rectangle, FPS, and benchmark counters.

## Performance Analysis

Benchmark mode separates capture wait, frame handover, decode, resize, segmentation preprocess, segmentation inference, segmentation postprocess, mask upscale, overlay, display, processing total, and pipeline total.

Example:

```bash
~/JONImageProcessor/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --benchmark --max-frames 300 --no-display --no-mask --no-overlay
```

This makes it visible whether runtime is spent waiting for camera frames, running segmentation, copying frames, rendering overlays, or displaying output.

## Planned Service Operation

Future versions should run automatically as a Linux service/daemon through systemd and display fullscreen to HDMI or DisplayPort.

Not implemented yet:

- no systemd unit
- no daemon mode
- no boot-time display setup

Long-term appliance flow:

```text
Jetson boots
-> systemd starts JONImageProcessor
-> HDMI resolution is detected
-> fullscreen output is opened
-> the image is centered correctly
-> display mode controls scaling
```

## Planned Runtime Control

Processing settings already live in a central `ProcessorConfig` structure. This prepares the application for future runtime updates.

Planned, but not implemented:

- local Unix domain socket
- JSON runtime commands
- background image control
- blur strength
- transparency
- fullscreen control
- mask debug overlay control
