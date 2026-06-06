# Cross-Compile for Jetson Orin Nano

This project can be built directly on the Jetson, which is the simplest path for camera and GPU validation. Cross-compilation from an x86_64 Linux host is possible, but the target sysroot must contain matching JetPack/L4T, OpenCV, CUDA, TensorRT, `jetson-inference`, and `jetson-utils` headers and libraries.

## Version Matching

Use a cross-compile container tag that matches the JetPack SDK version installed on the Jetson target. NVIDIA publishes the `jetpack-linux-aarch64-crosscompile-x86` NGC container for this purpose.

Examples:

```bash
docker pull nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

```bash
docker run --rm -it -v "$PWD":/workspace/JONImageProcessor -w /workspace/JONImageProcessor nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

If the Jetson uses a different JetPack/L4T release, replace `6.1` with the matching NGC tag.

## Dependencies

The build needs:

- CMake
- aarch64 C++17 toolchain
- target OpenCV headers and libraries
- CUDA runtime headers and libraries when `JON_ENABLE_JETSON_INFERENCE=ON`
- TensorRT through the JetPack installation used by `jetson-inference`
- `jetson-inference` headers and `libjetson-inference`
- `jetson-utils` headers and `libjetson-utils`

The NGC cross-compile container provides the cross-compile tools and JetPack build environment. It does not automatically know about every custom library installed on your Jetson. If `jetson-inference` is built manually on the Jetson, mirror its install prefix into the sysroot or build/install it inside the cross-compile environment for the aarch64 target.

## Configure Without Jetson Inference

This builds the portable OpenCV/V4L2 parts and the `none`/`dummy` mask backends:

```bash
cmake -B build-jetson -S . -DCMAKE_SYSTEM_PROCESSOR=aarch64
```

```bash
cmake --build build-jetson
```

## Configure With Jetson Inference

Use this when the aarch64 sysroot/container can find `jetson-inference`, `jetson-utils`, CUDA, TensorRT, and OpenCV:

```bash
cmake -B build-jetson -S . -DJON_ENABLE_JETSON_INFERENCE=ON -DCMAKE_SYSTEM_PROCESSOR=aarch64
```

```bash
cmake --build build-jetson
```

If CMake cannot find the Jetson inference headers or libraries, provide explicit search paths:

```bash
cmake -B build-jetson -S . -DJON_ENABLE_JETSON_INFERENCE=ON -DCMAKE_SYSTEM_PROCESSOR=aarch64 -DCMAKE_PREFIX_PATH="/usr/local;/usr" -DCMAKE_LIBRARY_PATH="/usr/local/lib;/usr/lib/aarch64-linux-gnu" -DCMAKE_INCLUDE_PATH="/usr/local/include;/usr/include"
```

## Runtime Validation

The x86_64 host can verify that the project configures and compiles for aarch64 when all target dependencies are present. It cannot validate Jetson GPU execution, TensorRT engine generation, `/dev/video0`, HDMI fullscreen behavior, or V4L2 camera timing unless those target devices and NVIDIA runtime libraries are available on the Jetson.

Run the real acceptance test on the Jetson:

```bash
./build/JONImageProcessor --device /dev/video0 --capture-backend v4l2 --camera-format MJPG --camera-fps 30 --width 1280 --height 720 --mask-backend jetson --background-overlay-color 0,0,255 --background-overlay-alpha 0.35 --benchmark
```

Expected first milestone:

- camera image is shown
- detected person remains unchanged
- background is tinted with the configured overlay color and alpha
- benchmark output reports capture wait, frame handover, resize, segmentation preprocess, segmentation inference, segmentation postprocess, mask upscale, overlay, display, processing total, and pipeline total

## Practical Recommendation

Build and smoke-test the portable path on x86_64 first. Build with `JON_ENABLE_JETSON_INFERENCE=ON` only in a JetPack-matched environment. Validate segmentation quality and FPS on the actual Jetson Orin Nano because TensorRT engine creation, GPU scheduling, camera timing, and display output are target-specific.
