#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

JETPACK_VERSION="${JETPACK_VERSION:-6.1}"
CONTAINER_IMAGE="${CONTAINER_IMAGE:-nvcr.io/nvidia/jetpack-linux-aarch64-crosscompile-x86:${JETPACK_VERSION}}"
BUILD_DIR_NAME="${BUILD_DIR_NAME:-build-jetson-cross}"
ENABLE_JETSON_INFERENCE="${ENABLE_JETSON_INFERENCE:-OFF}"
HOST_SYSROOT="${JETSON_SYSROOT:-}"
CONTAINER_SYSROOT=""
HOST_JETSON_INFERENCE_ROOT="${JETSON_INFERENCE_ROOT:-}"
CONTAINER_JETSON_INFERENCE_ROOT=""

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

if [[ -n "${HOST_JETSON_INFERENCE_ROOT}" ]]; then
    if [[ ! -d "${HOST_JETSON_INFERENCE_ROOT}" ]]; then
        echo "[ERROR] JETSON_INFERENCE_ROOT does not exist or is not a directory: ${HOST_JETSON_INFERENCE_ROOT}" >&2
        exit 2
    fi
    DOCKER_ARGS+=(-v "${HOST_JETSON_INFERENCE_ROOT}:/workspace/jetson-inference-root:ro")
    CONTAINER_JETSON_INFERENCE_ROOT="/workspace/jetson-inference-root"
fi

DOCKER_ARGS+=("${CONTAINER_IMAGE}")

CONTAINER_COMMAND=$(cat <<'EOS'
set -euo pipefail

echo "[INFO] Container: $(cat /etc/os-release | grep '^PRETTY_NAME=' | cut -d= -f2- | tr -d '"')"

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

if [[ -d /l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin ]]; then
    export JETSON_TOOLCHAIN_PREFIX="/l4t/toolchain/aarch64--glibc--stable-2022.08-1/bin/aarch64-buildroot-linux-gnu-"
elif [[ -d /l4t/toolchain/bin ]]; then
    export JETSON_TOOLCHAIN_PREFIX="/l4t/toolchain/bin/aarch64-buildroot-linux-gnu-"
fi

if [[ -n "${JETSON_TOOLCHAIN_PREFIX:-}" && ! -x "${JETSON_TOOLCHAIN_PREFIX}g++" ]]; then
    echo "[ERROR] Jetson cross compiler not found: ${JETSON_TOOLCHAIN_PREFIX}g++" >&2
    exit 4
fi

if [[ "${ENABLE_JETSON_INFERENCE}" == "ON" ]]; then
    if [[ -n "${JETSON_INFERENCE_ROOT}" ]]; then
        SEARCH_ROOTS=("${JETSON_INFERENCE_ROOT}" "${JETSON_SYSROOT}/usr/local" "${JETSON_SYSROOT}/usr")
    else
        SEARCH_ROOTS=("${JETSON_SYSROOT}/usr/local" "${JETSON_SYSROOT}/usr" "/usr/local" "/usr")
    fi

    FOUND_JETSON_INFERENCE=0
    for root in "${SEARCH_ROOTS[@]}"; do
        if [[ -f "${root}/include/jetson-inference/segNet.h" ]] && \
           { [[ -f "${root}/lib/libjetson-inference.so" ]] || [[ -f "${root}/lib/aarch64-linux-gnu/libjetson-inference.so" ]]; } && \
           { [[ -f "${root}/lib/libjetson-utils.so" ]] || [[ -f "${root}/lib/aarch64-linux-gnu/libjetson-utils.so" ]]; }; then
            FOUND_JETSON_INFERENCE=1
            break
        fi
    done

    if [[ "${FOUND_JETSON_INFERENCE}" != "1" ]]; then
        echo "[ERROR] ENABLE_JETSON_INFERENCE=ON, but jetson-inference dependencies were not found." >&2
        echo "[ERROR] Required: include/jetson-inference/segNet.h, libjetson-inference, libjetson-utils." >&2
        echo "[ERROR] Provide them in the sysroot/container or set JETSON_INFERENCE_ROOT=/path/to/aarch64/prefix." >&2
        exit 5
    fi
fi

if command -v git >/dev/null 2>&1; then
    git config --global --add safe.directory /workspace/JONImageProcessor
fi

rm -rf "${BUILD_DIR_NAME}/CMakeCache.txt" "${BUILD_DIR_NAME}/CMakeFiles"

cmake \
    -B "${BUILD_DIR_NAME}" \
    -S . \
    -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/jetson-aarch64.cmake \
    -DJON_ENABLE_JETSON_INFERENCE="${ENABLE_JETSON_INFERENCE}" \
    ${JETSON_INFERENCE_ROOT:+-DJON_JETSON_INFERENCE_ROOT="${JETSON_INFERENCE_ROOT}"}

cmake --build "${BUILD_DIR_NAME}" -- -j"$(nproc)"

if command -v file >/dev/null 2>&1; then
    file "${BUILD_DIR_NAME}/JONImageProcessor"
fi
EOS
)

echo "[INFO] Using container image: ${CONTAINER_IMAGE}"
echo "[INFO] Build directory: ${BUILD_DIR_NAME}"
echo "[INFO] Jetson inference: ${ENABLE_JETSON_INFERENCE}"
if [[ -n "${HOST_SYSROOT}" ]]; then
    echo "[INFO] Host sysroot: ${HOST_SYSROOT}"
else
    echo "[INFO] Host sysroot: auto from container /l4t/rootfs"
fi
if [[ -n "${HOST_JETSON_INFERENCE_ROOT}" ]]; then
    echo "[INFO] Host jetson-inference root: ${HOST_JETSON_INFERENCE_ROOT}"
fi

docker "${DOCKER_ARGS[@]}" /bin/bash -lc \
    "export BUILD_DIR_NAME='${BUILD_DIR_NAME}'; export ENABLE_JETSON_INFERENCE='${ENABLE_JETSON_INFERENCE}'; export JETSON_SYSROOT_IN_CONTAINER='${CONTAINER_SYSROOT}'; export JETSON_INFERENCE_ROOT='${CONTAINER_JETSON_INFERENCE_ROOT}'; ${CONTAINER_COMMAND}"
