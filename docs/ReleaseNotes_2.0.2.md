# JONImageProcessor 2.0.2 Release Notes

## Overview

Version 2.0.2 is a focused AirPlay performance and diagnostics release. It keeps
the AirPlay RTP architecture introduced in the 2.0.x series, but removes normal
runtime overhead from diagnostic paths that were only needed while debugging H264
stream quality and decoder behavior.

## Features

- AirPlay frame-content diagnostics are now verbose-only.
- Normal non-verbose operation no longer runs the expensive diagnostic frame
  analysis path.
- Secondary-camera black-frame detection now uses a cheaper mean-value check.
- Black-frame detection is throttled during normal AirPlay playback.
- Black-frame detection still checks every frame while no valid AirPlay frame is
  available or while a black-frame streak is active.
- AirPlay H264 debug dumping is now controlled by a compile-time build flag.
- Normal builds no longer include the AirPlay `tee/filesink` debug branch.
- `/tmp/jon-airplay-debug.h264` is no longer written unless explicitly enabled.
- New CMake option:
  - `JON_ENABLE_AIRPLAY_H264_DEBUG_DUMP`
- New cross-build environment switch:
  - `ENABLE_AIRPLAY_H264_DEBUG_DUMP=ON`
- Cross-build logging now reports whether the AirPlay H264 debug dump is enabled.
- README build documentation describes how to enable the optional AirPlay H264
  dump for diagnostics.

## Notes

- The default production build has `JON_ENABLE_AIRPLAY_H264_DEBUG_DUMP=OFF`.
- To capture the AirPlay H264 stream for debugging, build with
  `-DJON_ENABLE_AIRPLAY_H264_DEBUG_DUMP=ON` or set
  `ENABLE_AIRPLAY_H264_DEBUG_DUMP=ON` for the Jetson cross-build script.
- Version `2.0.2` is intended to reduce CPU load in normal AirPlay operation
  without changing the AirPlay RTP/decoder recovery architecture.
- The release tag is expected to be embedded in binaries built from the exact
  `2.0.2` tag.
