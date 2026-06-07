# Cross-Compile for Jetson Orin Nano

This document describes the preferred build path for producing a Jetson Orin Nano AArch64 binary from an x86_64 Linux VM. The native Jetson build is not the focus here.

The project provides:

- `cmake/toolchains/jetson-aarch64.cmake`
- `scripts/build-jetson-cross.sh`

The default cross-build intentionally does not enable `jetson-inference`. It builds the portable OpenCV/V4L2 code and the `none`/`dummy` mask backends first. Jetson/TensorRT segmentation is a separate opt-in step.

## 1. Check the Target JetPack/L4T Version

Run these commands on the Jetson target and record the output:

```bash
cat /etc/nv_tegra_release
```

```bash
dpkg-query -W nvidia-l4t-core
```

For Jetson Orin Nano, JetPack 6.1 corresponds to Jetson Linux/L4T 36.4. NVIDIA documents JetPack 6.1 as including Jetson Linux 36.4, CUDA 12.6, TensorRT 10.3, cuDNN 9.3, and support for Jetson Orin modules and developer kits.

Use a matching NGC cross-compile container tag. Example for JetPack 6.1:

```bash
export JETPACK_VERSION=6.1
```

The script uses this image by default:

```text
nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:${JETPACK_VERSION}
```

If your Jetson runs a different JetPack/L4T release, set `JETPACK_VERSION` to the matching container tag.

## 2. Dependency Sources

There are three possible dependency sources. Be explicit about which one you use.

### A. NVIDIA Container

The NGC cross-compile container provides the AArch64 cross toolchain and JetPack build environment. On first use, the script can extract `/l4t/targetfs.tbz2.*` into `/l4t/rootfs` inside the container when those files are present.

This is the simplest path when the container target rootfs already contains the needed development packages, especially OpenCV headers and libraries.

### B. Mounted Jetson Sysroot

You can mount a sysroot copied from the Jetson target:

```bash
export JETSON_SYSROOT=/path/to/jetson-sysroot
```

The sysroot must contain target headers and libraries, including OpenCV development files for the default build. For `JON_ENABLE_JETSON_INFERENCE=ON`, it must also contain CUDA/TensorRT-compatible `jetson-inference` and `jetson-utils` headers/libraries.

### C. Cross-Compiled jetson-inference

If `jetson-inference` is not present in the container or mounted sysroot, build and install it into an AArch64 prefix first, then point the build at that prefix:

```bash
export JETSON_INFERENCE_ROOT=/path/to/aarch64/jetson-inference-prefix
```

This prefix must contain:

```text
include/jetson-inference/segNet.h
lib/libjetson-inference.so
lib/libjetson-utils.so
```

or equivalent `lib/aarch64-linux-gnu` library paths.

## 3. Default Cross-Build Without jetson-inference

From the repository root on the x86_64 VM:

```bash
./scripts/build-jetson-cross.sh
```

Expected result:

```text
build-jetson-cross/JONImageProcessor
```

Verify the architecture:

```bash
file build-jetson-cross/JONImageProcessor
```

Expected architecture:

```text
ELF 64-bit ... ARM aarch64 ... Linux
```

This default build uses:

```text
JON_ENABLE_JETSON_INFERENCE=OFF
JON_ENABLE_TENSORRT_MASK=OFF
```

It should not require `jetson-inference` or `jetson-utils`.

## 4. Cross-Build With an Explicit Mounted Sysroot

If the container does not provide a complete target rootfs or OpenCV development files, mount your own sysroot:

```bash
JETSON_SYSROOT=/path/to/jetson-sysroot ./scripts/build-jetson-cross.sh
```

The script mounts it read-only at:

```text
/workspace/sysroot
```

The CMake toolchain then uses it as `CMAKE_SYSROOT` and `CMAKE_FIND_ROOT_PATH`.

## 5. Cross-Build With jetson-inference Enabled

Only enable this after the default cross-build works.

```bash
ENABLE_JETSON_INFERENCE=ON ./scripts/build-jetson-cross.sh
```

With an explicit sysroot:

```bash
JETSON_SYSROOT=/path/to/jetson-sysroot ENABLE_JETSON_INFERENCE=ON ./scripts/build-jetson-cross.sh
```

With a separate AArch64 `jetson-inference` install prefix:

```bash
JETSON_SYSROOT=/path/to/jetson-sysroot JETSON_INFERENCE_ROOT=/path/to/aarch64/jetson-inference-prefix ENABLE_JETSON_INFERENCE=ON ./scripts/build-jetson-cross.sh
```

When `ENABLE_JETSON_INFERENCE=ON`, the script also enables the generic TensorRT mask backend by default:

```text
ENABLE_TENSORRT_MASK=ON
```

The TensorRT mask backend requires `NvInfer.h`, `NvOnnxParser.h`, `libnvinfer`, and `libnvonnxparser` in the Jetson sysroot. Disable it explicitly if you only want the `jetson-inference` segNet path:

```bash
JETSON_SYSROOT=/path/to/jetson-sysroot JETSON_INFERENCE_ROOT=/path/to/aarch64/jetson-inference-prefix ENABLE_JETSON_INFERENCE=ON ENABLE_TENSORRT_MASK=OFF ./scripts/build-jetson-cross.sh
```

If the dependencies are missing, the script fails before CMake with a clear error:

```text
[ERROR] ENABLE_JETSON_INFERENCE=ON, but jetson-inference dependencies were not found.
[ERROR] Required: include/jetson-inference/segNet.h, libjetson-inference, libjetson-utils.
```

CMake also has a clear error if the preflight check is bypassed:

```text
JON_ENABLE_JETSON_INFERENCE=ON was requested, but jetson-inference/jetson-utils were not found.
```

## 6. Manual CMake Command Inside the Container

The script runs the equivalent of:

```bash
cmake -B build-jetson-cross -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/jetson-aarch64.cmake -DJON_ENABLE_JETSON_INFERENCE=OFF
```

```bash
cmake --build build-jetson-cross -- -j$(nproc)
```

For Jetson inference:

```bash
cmake -B build-jetson-cross -S . -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/jetson-aarch64.cmake -DJON_ENABLE_JETSON_INFERENCE=ON -DJON_ENABLE_TENSORRT_MASK=ON -DJON_JETSON_INFERENCE_ROOT=/path/to/aarch64/prefix
```

## 7. Runtime Validation

The x86_64 VM can validate that an AArch64 Linux binary is produced. It cannot validate:

- Jetson GPU execution
- TensorRT engine generation
- `/dev/video0`
- V4L2 live camera timing
- HDMI/DisplayPort fullscreen behavior

Run the runtime acceptance test on the Jetson:

```bash
./build/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend jetson --background-overlay-color 0,0,255 --background-overlay-alpha 0.35 --benchmark
```

Expected first milestone:

- camera image is shown
- detected person remains unchanged
- background is tinted with the configured overlay color and alpha
- benchmark output reports capture wait, frame handover, resize, segmentation preprocess, segmentation inference, segmentation postprocess, mask upscale, overlay, display, processing total, and pipeline total

## 8. Script Variables

The build script supports:

```text
JETPACK_VERSION=6.1
CONTAINER_IMAGE=nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
BUILD_DIR_NAME=build-jetson-cross
JETSON_SYSROOT=/path/to/jetson-sysroot
ENABLE_JETSON_INFERENCE=OFF
JETSON_INFERENCE_ROOT=/path/to/aarch64/jetson-inference-prefix
```

Examples:

```bash
JETPACK_VERSION=6.1 ./scripts/build-jetson-cross.sh
```

```bash
JETSON_SYSROOT=/opt/sysroots/orin-nano-jp61 ./scripts/build-jetson-cross.sh
```

```bash
JETSON_SYSROOT=/opt/sysroots/orin-nano-jp61 ENABLE_JETSON_INFERENCE=ON JETSON_INFERENCE_ROOT=/opt/aarch64/jetson-inference ./scripts/build-jetson-cross.sh
```
