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
- `--output-width <pixels>` and `--output-height <pixels>` explicitly define the display render surface. They must be specified together.
- `--width`, `--height`, `--mask-width`, and `--mask-height` configure processing and mask dimensions.
- `--version` prints the 7-character Git commit hash captured at CMake configure time. A semantic release version, such as `0.1.0`, is only printed when the build is configured exactly on a Git release tag like `v0.1.0` or `0.1.0`.

In window mode, `ESC` or `q` exits the program cleanly.

## Display Modes

The default display mode is `fit`.

- `fit` preserves the aspect ratio, keeps the complete image visible, centers it horizontally and vertically, and uses black bars when the display area has a different aspect ratio.
- `fill` preserves the aspect ratio, fills the complete display area, keeps the image centered, and crops overflowing image areas when needed.
- `stretch` ignores the aspect ratio and scales the image exactly to the display area. This avoids bars and cropping but may distort the image.

The application uses the current OpenCV window image area for display sizing when that value is reliable. If OpenCV returns an invalid or very small window image area, the renderer falls back to the configured display surface. Without an explicit display surface, that fallback is the processing size configured by `--width` and `--height`.

Use `--output-width` and `--output-height` to define the display render surface explicitly. This is useful on platforms where OpenCV HighGUI does not report the real fullscreen size reliably, especially on macOS. Fullscreen is still requested, but the render calculation can use the explicit size instead of depending on an unreliable window rectangle.

In verbose mode, display diagnostics include input frame size, processing size, window rectangle size, canvas size, display mode, and destination rectangle.

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

Verbose display diagnostics include input frame size, detected primary screen size, HighGUI window size, output canvas size, display mode, and destination rectangle. This makes platform-specific HighGUI behavior visible, especially when fullscreen window sizing differs between Linux and macOS. Performance diagnostics are emitted about once per second and include current FPS, average FPS, and processed frame count.

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
