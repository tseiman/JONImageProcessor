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
- `--width`, `--height`, `--mask-width`, and `--mask-height` configure processing and mask dimensions.

In window mode, `ESC` or `q` exits the program cleanly.

## Jetson Orin Nano Notes

This version intentionally uses only CMake, C++17, and OpenCV so it can build on a normal Linux VM and later on the Jetson Orin Nano.

On the Jetson, OpenCV and camera access should be verified separately before service operation. `/dev/video0` is the default for USB cameras. Depending on the camera, driver, and performance target, a GStreamer-based OpenCV pipeline may be useful later.

## Planned Service Operation

Future versions should run automatically as a Linux service/daemon through systemd and display the processed camera image fullscreen on HDMI or DisplayPort.

This initial step does not include a systemd unit, daemon mode, or display-specific initialization.

## Planned runtime control

Processing settings already live in a central `ProcessorConfig` structure. This structure is intentionally prepared so it can be updated at runtime later.

The planned approach is a local Unix domain socket that accepts JSON commands. Future commands should allow settings such as background image, blur strength, transparency, fullscreen, and mask debug overlay to be changed at runtime.

Not implemented yet:

- no Unix domain socket control
- no WebAPI
- no JSON runtime protocol

## Test Data

The `testdata/` directory is intended for local test videos and is not ignored globally.
