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

option(JON_ENABLE_JETSON_INFERENCE "Enable jetson-inference/TensorRT segmentation backend" OFF)
set(JON_JETSON_INFERENCE_ROOT "" CACHE PATH "Optional root/prefix containing jetson-inference and jetson-utils for cross builds")

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
endif()

cmake_host_system_information(RESULT JON_IMAGE_PROCESSOR_BUILD_HOST QUERY HOSTNAME)
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
    src/DisplayBackendFactory.cpp
    src/DisplayEnvironment.cpp
    src/DisplayRenderer.cpp
    src/Logger.cpp
    src/LowLatencyFrameCapture.cpp
    src/MaskBackendFactory.cpp
    src/MaskBackends.cpp
    src/JetsonSegmentationMaskBackend.cpp
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

if (JON_ENABLE_JETSON_INFERENCE)
    find_package(CUDAToolkit REQUIRED)

    if (JON_JETSON_INFERENCE_ROOT)
        find_path(JETSON_INFERENCE_INCLUDE_DIR
            NAMES jetson-inference/segNet.h
            PATHS ${JON_JETSON_INFERENCE_ROOT}
            PATH_SUFFIXES include
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
        find_library(JETSON_INFERENCE_LIBRARY
            NAMES jetson-inference
            PATHS ${JON_JETSON_INFERENCE_ROOT}
            PATH_SUFFIXES lib lib/aarch64-linux-gnu
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
        find_library(JETSON_UTILS_LIBRARY
            NAMES jetson-utils
            PATHS ${JON_JETSON_INFERENCE_ROOT}
            PATH_SUFFIXES lib lib/aarch64-linux-gnu
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
    else()
        find_path(JETSON_INFERENCE_INCLUDE_DIR
            NAMES jetson-inference/segNet.h
            PATHS /usr/local /usr
            PATH_SUFFIXES include
        )
        find_library(JETSON_INFERENCE_LIBRARY
            NAMES jetson-inference
            PATHS /usr/local /usr
            PATH_SUFFIXES lib lib/aarch64-linux-gnu
        )
        find_library(JETSON_UTILS_LIBRARY
            NAMES jetson-utils
            PATHS /usr/local /usr
            PATH_SUFFIXES lib lib/aarch64-linux-gnu
        )
    endif()

    if (NOT JETSON_INFERENCE_INCLUDE_DIR OR NOT JETSON_INFERENCE_LIBRARY OR NOT JETSON_UTILS_LIBRARY)
        message(FATAL_ERROR
            "JON_ENABLE_JETSON_INFERENCE=ON was requested, but jetson-inference/jetson-utils were not found.\n"
            "Expected header: jetson-inference/segNet.h\n"
            "Expected libraries: libjetson-inference and libjetson-utils\n"
            "Provide them from a Jetson sysroot, from the NGC container, or from a cross-compiled jetson-inference install.\n"
            "Set -DJON_JETSON_INFERENCE_ROOT=/path/to/prefix if they are installed outside /usr or /usr/local."
        )
    endif()

    find_library(JETSON_CUBLAS_LIBRARY
        NAMES cublas
        PATHS /usr/local/cuda-12.6/targets/aarch64-linux /usr/local/cuda/targets/aarch64-linux
        PATH_SUFFIXES lib lib64
    )
    find_library(JETSON_NPPICC_LIBRARY
        NAMES nppicc
        PATHS /usr/local/cuda-12.6/targets/aarch64-linux /usr/local/cuda/targets/aarch64-linux
        PATH_SUFFIXES lib lib64
    )
    find_library(JETSON_CUDLA_LIBRARY
        NAMES cudla
        PATHS /usr/local/cuda-12.6/targets/aarch64-linux /usr/local/cuda/targets/aarch64-linux
        PATH_SUFFIXES lib lib64
    )
    find_library(JETSON_NVDLA_COMPILER_LIBRARY
        NAMES nvdla_compiler
        PATHS /usr/lib/aarch64-linux-gnu/nvidia /lib/aarch64-linux-gnu/nvidia
    )

    if (NOT JETSON_CUBLAS_LIBRARY OR NOT JETSON_NPPICC_LIBRARY OR NOT JETSON_CUDLA_LIBRARY OR NOT JETSON_NVDLA_COMPILER_LIBRARY)
        message(FATAL_ERROR
            "JON_ENABLE_JETSON_INFERENCE=ON was requested, but required transitive Jetson libraries were not found.\n"
            "Expected libraries: libcublas, libnppicc, libcudla, libnvdla_compiler.\n"
            "Make sure the Jetson sysroot contains CUDA/TensorRT runtime and development libraries."
        )
    endif()

    target_include_directories(JONImageProcessor PRIVATE ${JETSON_INFERENCE_INCLUDE_DIR})
    target_link_libraries(JONImageProcessor PRIVATE
        ${JETSON_INFERENCE_LIBRARY}
        ${JETSON_UTILS_LIBRARY}
        CUDA::cudart
        ${JETSON_CUBLAS_LIBRARY}
        ${JETSON_NPPICC_LIBRARY}
        ${JETSON_CUDLA_LIBRARY}
        ${JETSON_NVDLA_COMPILER_LIBRARY}
    )
    target_compile_definitions(JONImageProcessor PRIVATE JON_WITH_JETSON_INFERENCE=1)
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
