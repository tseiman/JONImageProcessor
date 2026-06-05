cmake_minimum_required(VERSION 3.16)

project(JONImageProcessor
    VERSION 0.1.0
    DESCRIPTION "Jetson Orin Nano image processor"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCV REQUIRED COMPONENTS core imgproc highgui videoio)

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

add_executable(JONImageProcessor
    src/main.cpp
    src/BenchmarkRecorder.cpp
    src/CommandLineOptions.cpp
    src/DisplayEnvironment.cpp
    src/DisplayRenderer.cpp
    src/Logger.cpp
    src/VideoProcessor.cpp
)

target_include_directories(JONImageProcessor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${OpenCV_INCLUDE_DIRS}
)

target_link_libraries(JONImageProcessor PRIVATE ${OpenCV_LIBS})

target_compile_definitions(JONImageProcessor PRIVATE
    JON_IMAGE_PROCESSOR_GIT_VERSION="${JON_IMAGE_PROCESSOR_GIT_VERSION}"
    JON_IMAGE_PROCESSOR_RELEASE_VERSION="${JON_IMAGE_PROCESSOR_RELEASE_VERSION}"
)

if (APPLE)
    target_link_libraries(JONImageProcessor PRIVATE "-framework CoreGraphics")
endif()

if (MSVC)
    target_compile_options(JONImageProcessor PRIVATE /W4)
else()
    target_compile_options(JONImageProcessor PRIVATE -Wall -Wextra -Wpedantic)
endif()
