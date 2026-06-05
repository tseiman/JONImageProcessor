# JONImageProcessor

JONImageProcessor is a C++ base project for a Jetson Orin Nano image processor. The application is intended to read a camera image, process it, and display the processed output fullscreen over HDMI or DisplayPort.

This initial version provides the project foundation: CMake, OpenCV, command-line options, file or camera input, a simple dummy mask as a visible overlay, and output to either an OpenCV window or an MP4 file.

## Build

Linux requirements:

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

## Example Commands

```bash
./build/JONImageProcessor --help
```

```bash
./build/JONImageProcessor --version
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --output window
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --output file \
  --output-file output.mp4
```

```bash
./build/JONImageProcessor \
  --device /dev/video0 \
  --width 1280 \
  --height 720 \
  --mask-width 256 \
  --mask-height 144
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --display-mode fit
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --display-mode fill
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --display-mode stretch
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --fullscreen \
  --display-mode fit \
  --output-width 1920 \
  --output-height 1080 \
  --verbose
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  -v
```

```bash
./build/JONImageProcessor \
  --input testdata/Test1_pixabay_Video_4k.mp4 \
  --benchmark \
  --max-frames 500
```

```bash
./build/JONImageProcessor \
  --input testdata/Test1_pixabay_Video_4k.mp4 \
  --benchmark \
  --no-display \
  --no-mask \
  --no-overlay
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
- `--output-width <pixels>` and `--output-height <pixels>` explicitly define the display render surface. They must be specified together.
- `--width`, `--height`, `--mask-width`, and `--mask-height` configure processing and mask dimensions.
- `--camera-format <format>` requests a camera pixel format. Supported values are `MJPG` and `YUYV`; the default is `MJPG`.
- `--camera-fps <fps>` requests a camera frame rate. The default is `30`.
- `--version` prints the 7-character Git commit hash captured at CMake configure time. A semantic release version, such as `0.1.0`, is only printed when the build is configured exactly on a Git release tag like `v0.1.0` or `0.1.0`.
- `--benchmark` enables benchmark mode.
- `--max-frames <n>` stops automatically after n processed frames.
- `--no-display` disables window and file output.
- `--no-mask` disables dummy mask generation and mask upscaling.
- `--no-overlay` disables overlay rendering.

In window mode, `ESC` or `q` exits the program cleanly.

## Camera Configuration

When using `--device`, the application requests the configured camera pixel format, frame size, and frame rate through OpenCV. `--width` and `--height` are used both as processing size and requested camera capture size.

```bash
./build/JONImageProcessor \
  --device /dev/video0 \
  --width 1920 \
  --height 1080 \
  --camera-format MJPG \
  --camera-fps 30
```

```bash
./build/JONImageProcessor \
  --device /dev/video0 \
  --width 1280 \
  --height 720 \
  --camera-format MJPG \
  --camera-fps 60
```

Many USB webcams need MJPG for high resolutions and useful frame rates. Uncompressed YUYV often supports only very low frame rates at 1080p or above.

Verbose mode logs the requested camera settings and the active settings reported by OpenCV after configuration. If the camera does not accept the requested format, size, or FPS, a warning is emitted.

## Display Modes

The default display mode is `fit`.

- `fit` preserves the aspect ratio, keeps the complete image visible, centers it horizontally and vertically, and uses black bars when the display area has a different aspect ratio.
- `fill` preserves the aspect ratio, fills the complete display area, keeps the image centered, and crops overflowing image areas when needed.
- `stretch` ignores the aspect ratio and scales the image exactly to the display area. This avoids bars and cropping but may distort the image.

The application uses the current OpenCV window image area for display sizing when that value is reliable. In fullscreen mode, if the primary screen size is available, that screen size is used as the render canvas because OpenCV HighGUI can report stale or invalid window sizes on some platforms. If no reliable screen or window size is available, the renderer falls back to the configured display surface. Without an explicit display surface, that fallback is the processing size configured by `--width` and `--height`.

Use `--output-width` and `--output-height` to define the display render surface explicitly. This is useful on platforms where OpenCV HighGUI does not report the real fullscreen size reliably, especially on macOS. Fullscreen is still requested, but the render calculation can use the explicit size instead of depending on an unreliable window rectangle.

In verbose mode, display diagnostics include input frame size, processing size, window rectangle size, canvas size, display mode, and destination rectangle.

## Display Backends

The display path is separated from the video processing pipeline through an `IDisplayBackend` interface. This keeps decoding, resizing, mask generation, and overlay rendering independent from the final output system.

Currently supported backend:

- `highgui`: OpenCV HighGUI window output. This preserves the current behavior and remains the default.

The backend can be selected explicitly:

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --display-backend highgui
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
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  --verbose
```

```bash
./build/JONImageProcessor \
  --input testdata/Test2_pixabay_Video_HD.mp4 \
  -v
```

Verbose startup diagnostics include the release version state, Git version, build date, operating system, OpenCV version, input source, output mode, display mode, processing size, mask size, and fullscreen state.

Verbose display diagnostics include display input frame size, detected primary screen size, backend window size, output canvas size, display mode, and destination rectangle. This makes platform-specific backend behavior visible, especially when fullscreen window sizing differs between Linux and macOS. Performance diagnostics are emitted about once per second and include current FPS, average FPS, and processed frame count.

## Performance Analysis

Benchmark mode measures where frame time is spent without changing the processing pipeline by default:

```bash
./build/JONImageProcessor \
  --input testdata/Test1_pixabay_Video_4k.mp4 \
  --benchmark \
  --max-frames 500
```

Use `--max-frames <n>` to make benchmark runs repeatable and automatically stop after a fixed number of frames.

Use `--no-display` to skip window creation and file output. This isolates decode, resize, mask, and overlay cost:

```bash
./build/JONImageProcessor \
  --input testdata/Test1_pixabay_Video_4k.mp4 \
  --benchmark \
  --no-display \
  --max-frames 500
```

Use `--no-mask` to skip mask generation and mask upscaling:

```bash
./build/JONImageProcessor \
  --input testdata/Test1_pixabay_Video_4k.mp4 \
  --benchmark \
  --no-display \
  --no-mask \
  --max-frames 500
```

Use `--no-overlay` to skip overlay rendering:

```bash
./build/JONImageProcessor \
  --input testdata/Test1_pixabay_Video_4k.mp4 \
  --benchmark \
  --no-display \
  --no-mask \
  --no-overlay \
  --max-frames 500
```

Benchmark output reports average time for decode, resize, mask generation, mask upscale, overlay, display, total frame time, effective FPS, and percentage distribution including unmeasured overhead as `Other`.

The goal is to compare macOS development systems, Linux VMs, and Jetson Orin Nano runs objectively before deciding where optimization work should happen.

## Jetson Orin Nano Notes

This version intentionally uses only CMake, C++17, and OpenCV so it can build on a normal Linux VM and later on the Jetson Orin Nano.

On the Jetson, OpenCV and camera access should be verified separately before service operation. `/dev/video0` is the default for USB cameras. Depending on the camera, driver, and performance target, a GStreamer-based OpenCV pipeline may be useful later.

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
