cmake_minimum_required(VERSION 3.16)

project(JONImageProcessor
    VERSION 0.1.0
    DESCRIPTION "Jetson Orin Nano image processor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc imgcodecs highgui videoio)
find_package(PkgConfig QUIET)
if (PkgConfig_FOUND)
    pkg_check_modules(GSTREAMER_APP QUIET
        gstreamer-1.0
        gstreamer-app-1.0
        gstreamer-video-1.0
    )
    pkg_check_modules(WPE_HTML_RENDERER_11 QUIET
        wpe-webkit-1.1
        wpe-1.0
        wpebackend-fdo-1.0
        egl
        glesv2
        glib-2.0
        gobject-2.0
        gio-2.0
    )
    pkg_check_modules(WPE_HTML_RENDERER_10 QUIET
        wpe-webkit-1.0
        wpe-1.0
        wpebackend-fdo-1.0
        egl
        glesv2
        glib-2.0
        gobject-2.0
        gio-2.0
    )
    if (WPE_HTML_RENDERER_11_FOUND)
        set(WPE_HTML_RENDERER_FOUND TRUE)
        set(WPE_HTML_RENDERER_INCLUDE_DIRS ${WPE_HTML_RENDERER_11_INCLUDE_DIRS})
        set(WPE_HTML_RENDERER_LIBRARY_DIRS ${WPE_HTML_RENDERER_11_LIBRARY_DIRS})
        set(WPE_HTML_RENDERER_LIBRARIES ${WPE_HTML_RENDERER_11_LIBRARIES})
    elseif (WPE_HTML_RENDERER_10_FOUND)
        set(WPE_HTML_RENDERER_FOUND TRUE)
        set(WPE_HTML_RENDERER_INCLUDE_DIRS ${WPE_HTML_RENDERER_10_INCLUDE_DIRS})
        set(WPE_HTML_RENDERER_LIBRARY_DIRS ${WPE_HTML_RENDERER_10_LIBRARY_DIRS})
        set(WPE_HTML_RENDERER_LIBRARIES ${WPE_HTML_RENDERER_10_LIBRARIES})
    endif()
endif()
if (GSTREAMER_APP_FOUND)
    message(STATUS "GStreamer appsink secondary camera reader enabled")
else()
    message(STATUS "GStreamer appsink development files not found; secondary camera source will be unavailable")
endif()
if (WPE_HTML_RENDERER_FOUND)
    message(STATUS "WPE WebKit HTML media renderer enabled")
else()
    message(STATUS "WPE WebKit development files not found; HTML media files will be rejected")
endif()

find_path(FREETYPE_INCLUDE_DIR
    NAMES ft2build.h
    PATHS /usr/include/freetype2 /usr/include /usr/local/include /usr/local/include/freetype2
    PATH_SUFFIXES freetype2
    NO_CMAKE_FIND_ROOT_PATH
)
if (NOT FREETYPE_INCLUDE_DIR AND EXISTS "/usr/include/freetype2/ft2build.h")
    set(FREETYPE_INCLUDE_DIR "/usr/include/freetype2")
endif()
find_library(FREETYPE_LIBRARY
    NAMES freetype libfreetype.so.6
    PATHS /usr /usr/local
    PATH_SUFFIXES lib lib64 lib/aarch64-linux-gnu
)
if (NOT FREETYPE_LIBRARY AND CMAKE_SYSROOT)
    find_file(FREETYPE_LIBRARY
        NAMES libfreetype.so libfreetype.so.6
        PATHS
            "${CMAKE_SYSROOT}/usr/lib/aarch64-linux-gnu"
            "${CMAKE_SYSROOT}/lib/aarch64-linux-gnu"
            "${CMAKE_SYSROOT}/usr/lib"
            "${CMAKE_SYSROOT}/lib"
        NO_DEFAULT_PATH
    )
endif()
if (FREETYPE_INCLUDE_DIR AND FREETYPE_LIBRARY)
    message(STATUS "FreeType pause text rendering enabled")
else()
    message(STATUS "FreeType development files not found; pause text is limited to built-in Hershey fonts")
endif()

option(JON_ENABLE_TENSORRT_MASK "Enable generic TensorRT/ONNX mask backend" OFF)
option(JON_ENABLE_DRM_DISPLAY "Enable Linux DRM/KMS display backend" OFF)
option(JON_ENABLE_AIRPLAY_H264_DEBUG_DUMP "Write AirPlay RTP H264 debug stream to /tmp/jon-airplay-debug.h264" OFF)
set(JON_IMAGE_PROCESSOR_BUILD_HOST_OVERRIDE "" CACHE STRING "Optional build host name override, useful for containerized cross builds")

execute_process(
    COMMAND git rev-parse --short=7 HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE JON_IMAGE_PROCESSOR_GIT_VERSION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

if (NOT JON_IMAGE_PROCESSOR_GIT_VERSION)
    set(JON_IMAGE_PROCESSOR_GIT_VERSION "unknown")
endif()

execute_process(
    COMMAND git describe --tags --exact-match HEAD
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE JON_IMAGE_PROCESSOR_RELEASE_TAG
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)

set(JON_IMAGE_PROCESSOR_RELEASE_VERSION "")
if (JON_IMAGE_PROCESSOR_RELEASE_TAG MATCHES "^v?[0-9]+\\.[0-9]+\\.[0-9]+([-+][A-Za-z0-9._-]+)?$")
    string(REGEX REPLACE "^v" "" JON_IMAGE_PROCESSOR_RELEASE_VERSION "${JON_IMAGE_PROCESSOR_RELEASE_TAG}")
    set(JON_IMAGE_PROCESSOR_GIT_VERSION "${JON_IMAGE_PROCESSOR_RELEASE_VERSION}")
endif()

if (JON_IMAGE_PROCESSOR_BUILD_HOST_OVERRIDE)
    set(JON_IMAGE_PROCESSOR_BUILD_HOST "${JON_IMAGE_PROCESSOR_BUILD_HOST_OVERRIDE}")
else()
    cmake_host_system_information(RESULT JON_IMAGE_PROCESSOR_BUILD_HOST QUERY HOSTNAME)
endif()
if (NOT JON_IMAGE_PROCESSOR_BUILD_HOST)
    set(JON_IMAGE_PROCESSOR_BUILD_HOST "unknown")
endif()
string(REGEX REPLACE "\\..*$" "" JON_IMAGE_PROCESSOR_BUILD_HOST "${JON_IMAGE_PROCESSOR_BUILD_HOST}")
if (NOT JON_IMAGE_PROCESSOR_BUILD_HOST)
    set(JON_IMAGE_PROCESSOR_BUILD_HOST "unknown")
endif()

add_executable(JONImageProcessor
    src/main.cpp
    src/BenchmarkRecorder.cpp
    src/CaptureBackendFactory.cpp
    src/CommandLineOptions.cpp
    src/ConfigFile.cpp
    src/Daemon.cpp
    src/DisplayBackendFactory.cpp
    src/DisplayEnvironment.cpp
    src/DisplayRenderer.cpp
    src/DrmKmsDisplayBackend.cpp
    src/Logger.cpp
    src/LowLatencyFrameCapture.cpp
    src/HtmlMediaRenderer.cpp
    src/ipc/IPCServer.cpp
    src/ShutdownSignal.cpp
    src/TensorRtMaskBackend.cpp
    src/OpenCvDisplayBackend.cpp
    src/OpenCvCaptureBackends.cpp
    src/V4L2CameraCaptureBackend.cpp
    src/VideoProcessor.cpp
)

target_include_directories(JONImageProcessor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(JONImageProcessor PRIVATE ${OpenCV_LIBS})

if (GSTREAMER_APP_FOUND)
    target_compile_definitions(JONImageProcessor PRIVATE JON_ENABLE_GSTREAMER_APP=1)
    if (JON_ENABLE_AIRPLAY_H264_DEBUG_DUMP)
        target_compile_definitions(JONImageProcessor PRIVATE JON_ENABLE_AIRPLAY_H264_DEBUG_DUMP=1)
    endif()
    target_include_directories(JONImageProcessor PRIVATE ${GSTREAMER_APP_INCLUDE_DIRS})
    target_link_directories(JONImageProcessor PRIVATE ${GSTREAMER_APP_LIBRARY_DIRS})
    target_link_libraries(JONImageProcessor PRIVATE ${GSTREAMER_APP_LIBRARIES})
endif()

if (WPE_HTML_RENDERER_FOUND)
    target_compile_definitions(JONImageProcessor PRIVATE JON_ENABLE_WPE_HTML_RENDERER=1)
    target_include_directories(JONImageProcessor PRIVATE ${WPE_HTML_RENDERER_INCLUDE_DIRS})
    target_link_directories(JONImageProcessor PRIVATE ${WPE_HTML_RENDERER_LIBRARY_DIRS})
    target_link_libraries(JONImageProcessor PRIVATE ${WPE_HTML_RENDERER_LIBRARIES})
endif()

if (FREETYPE_INCLUDE_DIR AND FREETYPE_LIBRARY)
    target_compile_definitions(JONImageProcessor PRIVATE JON_ENABLE_FREETYPE_TEXT=1)
    target_include_directories(JONImageProcessor PRIVATE ${FREETYPE_INCLUDE_DIR})
    target_link_libraries(JONImageProcessor PRIVATE ${FREETYPE_LIBRARY})
endif()

if (JON_ENABLE_TENSORRT_MASK)
    find_package(CUDAToolkit REQUIRED)

    find_path(TENSORRT_INCLUDE_DIR
        NAMES NvInfer.h NvOnnxParser.h
        PATHS /usr /usr/local
        PATH_SUFFIXES include include/aarch64-linux-gnu
    )
    find_library(TENSORRT_INFER_LIBRARY
        NAMES nvinfer
        PATHS /usr /usr/local
        PATH_SUFFIXES lib lib64 lib/aarch64-linux-gnu
    )
    find_library(TENSORRT_ONNX_PARSER_LIBRARY
        NAMES nvonnxparser
        PATHS /usr /usr/local
        PATH_SUFFIXES lib lib64 lib/aarch64-linux-gnu
    )
    find_library(TENSORRT_CUDLA_LIBRARY
        NAMES cudla
        PATHS /usr/local/cuda-12.6/targets/aarch64-linux /usr/local/cuda/targets/aarch64-linux /usr /usr/local
        PATH_SUFFIXES lib lib64 lib/aarch64-linux-gnu
    )
    find_library(TENSORRT_NVDLA_COMPILER_LIBRARY
        NAMES nvdla_compiler
        PATHS /usr/lib/aarch64-linux-gnu/nvidia /lib/aarch64-linux-gnu/nvidia /usr /usr/local
        PATH_SUFFIXES lib lib64 lib/aarch64-linux-gnu lib/aarch64-linux-gnu/nvidia
    )

    if (NOT TENSORRT_INCLUDE_DIR OR NOT TENSORRT_INFER_LIBRARY OR NOT TENSORRT_ONNX_PARSER_LIBRARY OR NOT TENSORRT_CUDLA_LIBRARY OR NOT TENSORRT_NVDLA_COMPILER_LIBRARY)
        message(FATAL_ERROR
            "JON_ENABLE_TENSORRT_MASK=ON was requested, but TensorRT development files were not found.\n"
            "Expected headers: NvInfer.h and NvOnnxParser.h\n"
            "Expected libraries: libnvinfer, libnvonnxparser, libcudla, and libnvdla_compiler\n"
            "Provide them from the Jetson sysroot or the NVIDIA cross-compile container."
        )
    endif()

    target_include_directories(JONImageProcessor PRIVATE ${TENSORRT_INCLUDE_DIR})
    target_link_libraries(JONImageProcessor PRIVATE
        CUDA::cudart
        ${TENSORRT_INFER_LIBRARY}
        ${TENSORRT_ONNX_PARSER_LIBRARY}
        ${TENSORRT_CUDLA_LIBRARY}
        ${TENSORRT_NVDLA_COMPILER_LIBRARY}
    )
    target_compile_definitions(JONImageProcessor PRIVATE JON_WITH_TENSORRT_MASK=1)
endif()

if (JON_ENABLE_DRM_DISPLAY)
    find_path(DRM_MODE_INCLUDE_DIR
        NAMES xf86drmMode.h
        PATH_SUFFIXES include include/libdrm
    )
    find_path(DRM_CORE_INCLUDE_DIR
        NAMES drm.h
        PATH_SUFFIXES include/libdrm include/drm include
    )
    find_path(GBM_INCLUDE_DIR
        NAMES gbm.h
        PATH_SUFFIXES include
    )
    find_path(EGL_INCLUDE_DIR
        NAMES EGL/egl.h
        PATH_SUFFIXES include
    )
    find_path(GLES2_INCLUDE_DIR
        NAMES GLES2/gl2.h
        PATH_SUFFIXES include
    )
    set(JON_DRM_LIBRARY_SEARCH_PATHS
        /usr/lib/aarch64-linux-gnu
        /lib/aarch64-linux-gnu
        /usr/lib
        /lib
    )
    find_library(DRM_LIBRARY NAMES drm PATHS ${JON_DRM_LIBRARY_SEARCH_PATHS})
    find_library(GBM_LIBRARY NAMES gbm PATHS ${JON_DRM_LIBRARY_SEARCH_PATHS})
    find_library(EGL_LIBRARY NAMES EGL PATHS ${JON_DRM_LIBRARY_SEARCH_PATHS})
    find_library(GLES2_LIBRARY NAMES GLESv2 PATHS ${JON_DRM_LIBRARY_SEARCH_PATHS})

    if (NOT DRM_MODE_INCLUDE_DIR OR NOT DRM_CORE_INCLUDE_DIR OR NOT GBM_INCLUDE_DIR OR NOT EGL_INCLUDE_DIR OR NOT GLES2_INCLUDE_DIR
        OR NOT DRM_LIBRARY OR NOT GBM_LIBRARY OR NOT EGL_LIBRARY OR NOT GLES2_LIBRARY)
        message(FATAL_ERROR
            "JON_ENABLE_DRM_DISPLAY=ON, but DRM/KMS dependencies were not found. "
            "Required: libdrm headers/library, gbm, EGL, GLESv2.")
    endif()

    target_compile_definitions(JONImageProcessor PRIVATE JON_ENABLE_DRM_DISPLAY=1)
    target_include_directories(JONImageProcessor PRIVATE
        ${DRM_MODE_INCLUDE_DIR}
        ${DRM_CORE_INCLUDE_DIR}
        ${GBM_INCLUDE_DIR}
        ${EGL_INCLUDE_DIR}
        ${GLES2_INCLUDE_DIR}
    )
    target_link_libraries(JONImageProcessor PRIVATE
        ${DRM_LIBRARY}
        ${GBM_LIBRARY}
        ${EGL_LIBRARY}
        ${GLES2_LIBRARY}
    )
endif()

target_compile_definitions(JONImageProcessor PRIVATE
    JON_IMAGE_PROCESSOR_GIT_VERSION="${JON_IMAGE_PROCESSOR_GIT_VERSION}"
    JON_IMAGE_PROCESSOR_RELEASE_VERSION="${JON_IMAGE_PROCESSOR_RELEASE_VERSION}"
    JON_IMAGE_PROCESSOR_BUILD_HOST="${JON_IMAGE_PROCESSOR_BUILD_HOST}"
)

if (APPLE)
    target_link_libraries(JONImageProcessor PRIVATE "-framework CoreGraphics")
endif()

if (MSVC)
    target_compile_options(JONImageProcessor PRIVATE /W4)
else()
    target_compile_options(JONImageProcessor PRIVATE -Wall -Wextra -Wpedantic)
endif()
