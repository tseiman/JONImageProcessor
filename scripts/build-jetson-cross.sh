#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

JETPACK_VERSION="${JETPACK_VERSION:-6.1}"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:${JETPACK_VERSION}}"
BUILD_DIR_NAME="${BUILD_DIR_NAME:-build-jetson-cross}"
ENABLE_TENSORRT_MASK="${ENABLE_TENSORRT_MASK:-ON}"
ENABLE_DRM_DISPLAY="${ENABLE_DRM_DISPLAY:-ON}"
INSTALL_WPE_DEV="${INSTALL_WPE_DEV:-ON}"
HOST_SYSROOT="${JETSON_SYSROOT:-}"
CONTAINER_SYSROOT=""
HOST_UID="$(id -u)"
HOST_GID="$(id -g)"
BUILD_HOST_NAME="${BUILD_HOST_NAME:-$(hostname -s 2>/dev/null || hostname 2>/dev/null || echo unknown)}"

DOCKER_ARGS=(
    run
    --rm
    -t
    -v "${REPO_ROOT}:/workspace/JONImageProcessor"
    -w /workspace/JONImageProcessor
)

if [[ -n "${HOST_SYSROOT}" ]]; then
    if [[ ! -d "${HOST_SYSROOT}" ]]; then
        echo "[ERROR] JETSON_SYSROOT does not exist or is not a directory: ${HOST_SYSROOT}" >&2
        exit 2
    fi
    DOCKER_ARGS+=(-v "${HOST_SYSROOT}:/workspace/sysroot:ro")
    CONTAINER_SYSROOT="/workspace/sysroot"
fi

DOCKER_ARGS+=("${CONTAINER_IMAGE}")

CONTAINER_COMMAND=$(cat <<'EOS'
set -euo pipefail

echo "[INFO] Container: $(cat /etc/os-release | grep '^PRETTY_NAME=' | cut -d= -f2- | tr -d '"')"

if [[ "${INSTALL_WPE_DEV}" == "ON" ]]; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update
    if apt-cache show libwpewebkit-1.1-dev >/dev/null 2>&1; then
        apt-get install -y --no-install-recommends libwpewebkit-1.1-dev libwpebackend-fdo-1.0-dev
    else
        apt-get install -y --no-install-recommends libwpewebkit-1.0-dev libwpebackend-fdo-1.0-dev
    fi
fi

if [[ -z "${JETSON_SYSROOT_IN_CONTAINER}" && -d /l4t && ! -d /l4t/rootfs && -f /l4t/targetfs.tbz2.00 ]]; then
    echo "[INFO] Extracting /l4t targetfs fragments into the container. This can take several minutes."
    cd /l4t
    cat targetfs.tbz2.* > targetfs.tbz2
    tar -I lbzip2 -xf targetfs.tbz2
fi

if [[ -z "${JETSON_SYSROOT_IN_CONTAINER}" && -d /l4t/rootfs ]]; then
    export JETSON_SYSROOT="/l4t/rootfs"
elif [[ -n "${JETSON_SYSROOT_IN_CONTAINER}" ]]; then
    export JETSON_SYSROOT="${JETSON_SYSROOT_IN_CONTAINER}"
fi

if [[ -z "${JETSON_SYSROOT:-}" || ! -d "${JETSON_SYSROOT}" ]]; then
    echo "[ERROR] No Jetson target sysroot found." >&2
    echo "[ERROR] Provide one with JETSON_SYSROOT=/path/to/jetson-sysroot or use a container that provides /l4t/rootfs." >&2
    exit 3
fi

export PKG_CONFIG_SYSROOT_DIR="${JETSON_SYSROOT}"
export PKG_CONFIG_LIBDIR="${JETSON_SYSROOT}/usr/lib/aarch64-linux-gnu/pkgconfig:${JETSON_SYSROOT}/usr/lib/pkgconfig:${JETSON_SYSROOT}/usr/share/pkgconfig:${JETSON_SYSROOT}/lib/aarch64-linux-gnu/pkgconfig:${JETSON_SYSROOT}/lib/pkgconfig"

if [[ -d /l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin ]]; then
    export JETSON_TOOLCHAIN_PREFIX="/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin/aarch64-buildroot-linux-gnu-"
elif [[ -d /l4t/toolchain/bin ]]; then
    export JETSON_TOOLCHAIN_PREFIX="/l4t/toolchain/bin/aarch64-buildroot-linux-gnu-"
fi

if [[ -n "${JETSON_TOOLCHAIN_PREFIX:-}" && ! -x "${JETSON_TOOLCHAIN_PREFIX}g++" ]]; then
    echo "[ERROR] Jetson cross compiler not found: ${JETSON_TOOLCHAIN_PREFIX}g++" >&2
    exit 4
fi

if [[ "${ENABLE_TENSORRT_MASK}" == "ON" ]]; then
    if [[ ! -f "${JETSON_SYSROOT}/usr/include/aarch64-linux-gnu/NvInfer.h" && ! -f "${JETSON_SYSROOT}/usr/include/NvInfer.h" ]]; then
        echo "[ERROR] ENABLE_TENSORRT_MASK=ON, but NvInfer.h was not found in the Jetson sysroot." >&2
        exit 6
    fi
    if ! find "${JETSON_SYSROOT}/usr/lib" "${JETSON_SYSROOT}/lib" -name 'libnvinfer.so*' -print -quit 2>/dev/null | grep -q .; then
        echo "[ERROR] ENABLE_TENSORRT_MASK=ON, but libnvinfer was not found in the Jetson sysroot." >&2
        exit 6
    fi
    if ! find "${JETSON_SYSROOT}/usr/lib" "${JETSON_SYSROOT}/lib" -name 'libnvonnxparser.so*' -print -quit 2>/dev/null | grep -q .; then
        echo "[ERROR] ENABLE_TENSORRT_MASK=ON, but libnvonnxparser was not found in the Jetson sysroot." >&2
        exit 6
    fi
    if ! find "${JETSON_SYSROOT}/usr/local/cuda"* "${JETSON_SYSROOT}/usr/lib" "${JETSON_SYSROOT}/lib" -name 'libcudla.so*' -print -quit 2>/dev/null | grep -q .; then
        echo "[ERROR] ENABLE_TENSORRT_MASK=ON, but libcudla was not found in the Jetson sysroot." >&2
        exit 6
    fi
    if ! find "${JETSON_SYSROOT}/usr/lib" "${JETSON_SYSROOT}/lib" -name 'libnvdla_compiler.so*' -print -quit 2>/dev/null | grep -q .; then
        echo "[ERROR] ENABLE_TENSORRT_MASK=ON, but libnvdla_compiler was not found in the Jetson sysroot." >&2
        exit 6
    fi
fi

if [[ "${ENABLE_DRM_DISPLAY}" == "ON" ]]; then
    if [[ ! -f "${JETSON_SYSROOT}/usr/include/xf86drmMode.h" && ! -f "${JETSON_SYSROOT}/usr/include/libdrm/xf86drmMode.h" ]]; then
        echo "[ERROR] ENABLE_DRM_DISPLAY=ON, but xf86drmMode.h was not found in the Jetson sysroot." >&2
        exit 7
    fi
    if [[ ! -f "${JETSON_SYSROOT}/usr/include/gbm.h" ]]; then
        echo "[ERROR] ENABLE_DRM_DISPLAY=ON, but gbm.h was not found in the Jetson sysroot." >&2
        exit 7
    fi
    if [[ ! -f "${JETSON_SYSROOT}/usr/include/EGL/egl.h" || ! -f "${JETSON_SYSROOT}/usr/include/GLES2/gl2.h" ]]; then
        echo "[ERROR] ENABLE_DRM_DISPLAY=ON, but EGL/GLES2 headers were not found in the Jetson sysroot." >&2
        exit 7
    fi
    for library in libdrm libgbm libEGL libGLESv2; do
        if ! find "${JETSON_SYSROOT}/usr/lib" "${JETSON_SYSROOT}/lib" -name "${library}.so*" -print -quit 2>/dev/null | grep -q .; then
            echo "[ERROR] ENABLE_DRM_DISPLAY=ON, but ${library} was not found in the Jetson sysroot." >&2
            exit 7
        fi
    done
fi

if command -v git >/dev/null 2>&1; then
    git config --global --add safe.directory /workspace/JONImageProcessor
fi

rm -rf "${BUILD_DIR_NAME}/CMakeCache.txt" "${BUILD_DIR_NAME}/CMakeFiles"

cmake \
    -B "${BUILD_DIR_NAME}" \
    -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/jetson-aarch64.cmake \
    -DJON_ENABLE_TENSORRT_MASK="${ENABLE_TENSORRT_MASK}" \
    -DJON_ENABLE_DRM_DISPLAY="${ENABLE_DRM_DISPLAY}" \
    -DJON_IMAGE_PROCESSOR_BUILD_HOST_OVERRIDE="${BUILD_HOST_NAME}"

cmake --build "${BUILD_DIR_NAME}" -- -j"$(nproc)"

if [[ -n "${HOST_UID:-}" && -n "${HOST_GID:-}" ]]; then
    chown -R "${HOST_UID}:${HOST_GID}" "${BUILD_DIR_NAME}" 2>/dev/null || true
fi

if command -v file >/dev/null 2>&1; then
    file "${BUILD_DIR_NAME}/JONImageProcessor"
fi
EOS
)

echo "[INFO] Using container image: ${CONTAINER_IMAGE}"
echo "[INFO] Build directory: ${BUILD_DIR_NAME}"
echo "[INFO] TensorRT mask backend: ${ENABLE_TENSORRT_MASK}"
echo "[INFO] DRM/KMS display backend: ${ENABLE_DRM_DISPLAY}"
echo "[INFO] Install WPE dev packages in container: ${INSTALL_WPE_DEV}"
echo "[INFO] Build host name: ${BUILD_HOST_NAME}"
if [[ -n "${HOST_SYSROOT}" ]]; then
    echo "[INFO] Host sysroot: ${HOST_SYSROOT}"
else
    echo "[INFO] Host sysroot: auto from container /l4t/rootfs"
fi
docker "${DOCKER_ARGS[@]}" /bin/bash -lc \
    "export BUILD_DIR_NAME='${BUILD_DIR_NAME}'; export ENABLE_TENSORRT_MASK='${ENABLE_TENSORRT_MASK}'; export ENABLE_DRM_DISPLAY='${ENABLE_DRM_DISPLAY}'; export INSTALL_WPE_DEV='${INSTALL_WPE_DEV}'; export BUILD_HOST_NAME='${BUILD_HOST_NAME}'; export JETSON_SYSROOT_IN_CONTAINER='${CONTAINER_SYSROOT}'; export HOST_UID='${HOST_UID}'; export HOST_GID='${HOST_GID}'; ${CONTAINER_COMMAND}"
