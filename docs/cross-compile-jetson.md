# Jetson Cross Compile

This project uses the NVIDIA NGC JetPack cross-compile container to build an AArch64 binary for Jetson Orin Nano from an x86_64 Linux build host.

## Prerequisites

On the build host:

```bash
docker pull nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:6.1
```

Create a target sysroot from the Jetson:

```bash
mkdir -p "$HOME/sysroots/orin-nano" && rsync -aHAX --numeric-ids tseiman@jon:/usr "$HOME/sysroots/orin-nano/" && rsync -aHAX --numeric-ids tseiman@jon:/lib "$HOME/sysroots/orin-nano/" && rsync -aHAX --numeric-ids tseiman@jon:/opt "$HOME/sysroots/orin-nano/"
```

Verify the required files:

```bash
find "$HOME/sysroots/orin-nano/usr" -name OpenCVConfig.cmake -o -name opencv4.pc
```

```bash
find "$HOME/sysroots/orin-nano/usr" "$HOME/sysroots/orin-nano/lib" -name NvInfer.h -o -name 'libnvinfer.so*' -o -name 'libnvonnxparser.so*'
```

```bash
find "$HOME/sysroots/orin-nano/usr" "$HOME/sysroots/orin-nano/lib" -name xf86drmMode.h -o -name gbm.h -o -name 'libdrm.so*' -o -name 'libgbm.so*'
```

For HTML background or pause media, install WPE runtime and development packages on the Jetson and sync the sysroot again:

```bash
sudo apt-get install libwpewebkit-1.1-0 libwpewebkit-1.1-dev libwpebackend-fdo-1.0-1 libwpebackend-fdo-1.0-dev
```

If the Jetson distribution only provides `libwpewebkit-1.0-*`, use the matching `1.0` package names instead.

Then verify the sysroot contains WPE target files:

```bash
find "$HOME/sysroots/orin-nano/usr" "$HOME/sysroots/orin-nano/lib" -name 'wpe-webkit-*.pc' -o -name 'wpebackend-fdo-1.0.pc' -o -name 'libWPEWebKit*.so*' -o -name 'libWPEBackend-fdo-1.0.so*'
```

## Build

From the repository root on the build host:

```bash
ENABLE_TENSORRT_MASK=ON ENABLE_DRM_DISPLAY=ON JETSON_SYSROOT="$HOME/sysroots/orin-nano" ./scripts/build-jetson-cross.sh
```

The script installs WPE development packages inside the container by default. CMake still uses only the AArch64 files from `JETSON_SYSROOT` for linking, so HTML support is enabled only when the synced Jetson sysroot contains WPE WebKit and WPEBackend-fdo target files.

The result is:

```bash
build-jetson-cross/JONImageProcessor
```

Verify:

```bash
file build-jetson-cross/JONImageProcessor
```

Expected: AArch64/Linux executable.

## Deploy

Run on the build host:

```bash
scp build-jetson-cross/JONImageProcessor tseiman@jon:~/JONImageProcessor/JONImageProcessor
```

Run on the Jetson:

```bash
chmod +x ~/JONImageProcessor/JONImageProcessor
```

## Run On Jetson

```bash
cd ~/JONImageProcessor && MODEL_PATH="$HOME/JONImageProcessor/models/modnet_photographic_portrait_matting.onnx" ./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --blur-strength 85 --display-backend drm --fullscreen --benchmark
```

GPU/TensorRT execution can only be tested on the Jetson. The build host only produces the target binary.
