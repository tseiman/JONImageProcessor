set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

set(_JETSON_TOOLCHAIN_PREFIX "$ENV{JETSON_TOOLCHAIN_PREFIX}")
set(_JETSON_SYSROOT "$ENV{JETSON_SYSROOT}")

if (NOT _JETSON_TOOLCHAIN_PREFIX)
    if (EXISTS "/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin/aarch64-buildroot-linux-gnu-g++")
        set(_JETSON_TOOLCHAIN_PREFIX "/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin/aarch64-buildroot-linux-gnu-")
    elseif (EXISTS "/l4t/toolchain/bin/aarch64-buildroot-linux-gnu-g++")
        set(_JETSON_TOOLCHAIN_PREFIX "/l4t/toolchain/bin/aarch64-buildroot-linux-gnu-")
    elseif (EXISTS "/usr/bin/aarch64-linux-gnu-g++")
        set(_JETSON_TOOLCHAIN_PREFIX "/usr/bin/aarch64-linux-gnu-")
    else()
        set(_JETSON_TOOLCHAIN_PREFIX "aarch64-linux-gnu-")
    endif()
endif()

if (NOT _JETSON_SYSROOT)
    if (EXISTS "/workspace/sysroot")
        set(_JETSON_SYSROOT "/workspace/sysroot")
    elseif (EXISTS "/l4t/rootfs")
        set(_JETSON_SYSROOT "/l4t/rootfs")
    endif()
endif()

set(CMAKE_C_COMPILER "${_JETSON_TOOLCHAIN_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${_JETSON_TOOLCHAIN_PREFIX}g++")

if (EXISTS "/usr/bin/gmake")
    set(CMAKE_MAKE_PROGRAM "/usr/bin/gmake" CACHE FILEPATH "Host make program" FORCE)
elseif (EXISTS "/usr/bin/make")
    set(CMAKE_MAKE_PROGRAM "/usr/bin/make" CACHE FILEPATH "Host make program" FORCE)
endif()

if (EXISTS "/usr/bin/pkg-config")
    set(PKG_CONFIG_EXECUTABLE "/usr/bin/pkg-config" CACHE FILEPATH "Host pkg-config program" FORCE)
endif()

if (_JETSON_SYSROOT)
    set(CMAKE_SYSROOT "${_JETSON_SYSROOT}")
    set(CMAKE_FIND_ROOT_PATH "${_JETSON_SYSROOT}")
    set(CMAKE_PREFIX_PATH
        "${_JETSON_SYSROOT}/usr/local"
        "${_JETSON_SYSROOT}/usr"
        ${CMAKE_PREFIX_PATH}
    )
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
