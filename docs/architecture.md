# Architecture

This document describes the current production-oriented path only: V4L2 camera input, TensorRT mask generation, background effect rendering, and DRM/KMS or HighGUI display output.

## Main Classes

```mermaid
classDiagram
    class main {
        +parseCommandLine()
        +VideoProcessor::run()
    }

    class ProcessorConfig {
        +inputPath
        +devicePath
        +width
        +height
        +maskModelPath
        +segmentationWidth
        +segmentationHeight
        +backgroundEffect
        +displayBackend
    }

    class VideoProcessor {
        -ProcessorConfig config
        +run() int
    }

    class ICaptureBackend {
        <<interface>>
        +open(config) bool
        +read(frame) bool
        +close()
    }

    class V4L2CameraCaptureBackend
    class OpenCvFileCaptureBackend

    class LowLatencyFrameCapture {
        +start(capture)
        +waitForLatestFrame(frame)
        +stop()
    }

    class IMaskBackend {
        <<interface>>
        +initialize(config) bool
        +generate(frame, index, mask, timings) bool
    }

    class TensorRtMaskBackend

    class IDisplayBackend {
        <<interface>>
        +initialize(config) bool
        +render(frame) bool
        +shutdown()
    }

    class DrmKmsDisplayBackend
    class OpenCvDisplayBackend

    main --> ProcessorConfig
    main --> VideoProcessor
    VideoProcessor --> ICaptureBackend
    ICaptureBackend <|.. V4L2CameraCaptureBackend
    ICaptureBackend <|.. OpenCvFileCaptureBackend
    VideoProcessor --> LowLatencyFrameCapture
    VideoProcessor --> IMaskBackend
    IMaskBackend <|.. TensorRtMaskBackend
    VideoProcessor --> IDisplayBackend
    IDisplayBackend <|.. DrmKmsDisplayBackend
    IDisplayBackend <|.. OpenCvDisplayBackend
```

## Standard Call Flow

Typical DRM blur call:

```bash
./JONImageProcessor --device /dev/video0 --processing-size 1280x720 --mask-model "$MODEL_PATH" --segmentation-size 384x384 --background-effect blur --display-backend drm --fullscreen --benchmark
```

```mermaid
flowchart TD
    A[main] --> B[parse CLI into ProcessorConfig]
    B --> C[VideoProcessor::run]
    C --> D[Create V4L2CameraCaptureBackend]
    D --> E[Start LowLatencyFrameCapture thread]
    C --> F[Create TensorRtMaskBackend]
    F --> G[Load cached engine or build engine from ONNX]
    C --> H[Create DrmKmsDisplayBackend]
    E --> I[Newest camera frame]
    I --> J[Resize to processing size]
    J --> K[TensorRT mask inference]
    K --> L[Mask smoothing and morphology]
    L --> M[Apply background blur, color, or image]
    M --> N[Render frame through DRM/KMS]
    N --> I
```

For `--input <path>`, `OpenCvFileCaptureBackend` is used instead of V4L2 and frames are processed sequentially. For `--display-backend highgui`, `OpenCvDisplayBackend` replaces the DRM/KMS backend.

For `--background-effect image`, the image is loaded once at startup and resized to the processed output frame size before compositing.

## Ownership Boundaries

- `CommandLineOptions` owns CLI parsing and validation only.
- `VideoProcessor` owns the frame pipeline and timing.
- Capture backends only deliver BGR `cv::Mat` frames.
- `TensorRtMaskBackend` owns TensorRT engine loading/building and mask inference.
- Display backends only render the already composited output frame.

## Model And Engine Files

The ONNX model is portable and should be treated as the source model. The generated `.engine` file is a TensorRT plan optimized for a specific TensorRT/CUDA/GPU environment and input size. It can be cached on the Jetson to avoid repeated startup builds, but it should not be treated as a universal artifact.
