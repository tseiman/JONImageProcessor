# JONImageProcessor 1.0.0 Release Notes

## Overview

Version 1.0.0 is the first production-oriented release of JONImageProcessor for NVIDIA Jetson Orin Nano systems. It provides a C++ video-processing pipeline for low-latency camera input, TensorRT-based person segmentation, configurable background rendering, local runtime control, and appliance-style display output.

## Features

- CMake-based C++17 project structure for `JONImageProcessor`.
- V4L2 camera capture backend with MJPG and YUYV support.
- Low-latency camera capture that keeps the newest available frame instead of queueing old frames.
- OpenCV video file input for development and testing.
- TensorRT mask backend for person/background segmentation.
- DRM/KMS display backend for appliance-style fullscreen output.
- HighGUI display backend for development use.
- Background effects:
  - `none`
  - `color`
  - `blur`
  - `image`
- Background media support for:
  - static images
  - video files
  - HTML/CSS/JavaScript via WPE WebKit
- Pause/status screen support with either a generated test pattern or configured image/video/HTML media.
- Runtime camera status screens:
  - `Camera OFF`
  - `Camera connecting...`
  - `Camera DISCONNECTED`
- USB camera disconnect/reconnect handling.
- JSON configuration file support with CLI overrides.
- Unix domain socket IPC using NDJSON.
- IPC commands:
  - `get`
  - `set`
  - `list`
- Runtime updates for segmentation, background, pause screen, camera enabled state, and benchmark access.
- Benchmark collection for capture, segmentation, mask processing, overlay, display, and frame timing.
- Foreground and systemd-friendly runtime behavior with clean signal handling.
- systemd service example and deployment documentation.
- Cross-compile support for Jetson Orin Nano using the NVIDIA JetPack cross-compile container.
- Git-version patching into the produced AArch64 binary.
- README and architecture documentation for build, deployment, configuration, IPC, media handling, and runtime behavior.

## Notes

- The primary production path is V4L2 camera input, TensorRT segmentation, DRM/KMS display output, and JSON-based configuration.
- HTML media requires a build with WPE WebKit support and matching runtime libraries on the target system.
- Benchmark log output is intended for diagnostics and can be controlled separately from benchmark data collection.
