#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 1 || $# -gt 3 ]]; then
    echo "Usage: $0 <binary> [new-git-hash] [old-git-hash]" >&2
    exit 2
fi

BINARY="$1"
NEW_HASH="${2:-}"
OLD_HASH="${3:-}"

if [[ ! -f "${BINARY}" ]]; then
    echo "[ERROR] Binary not found: ${BINARY}" >&2
    exit 2
fi

if [[ -z "${NEW_HASH}" ]]; then
    RELEASE_TAG="$(git describe --tags --exact-match HEAD 2>/dev/null || true)"
    if [[ "${RELEASE_TAG}" =~ ^v?[0-9]+\.[0-9]+\.[0-9]+([-+][A-Za-z0-9._-]+)?$ ]]; then
        echo "[INFO] Release tag ${RELEASE_TAG#v} detected; not patching a dev git hash into ${BINARY}"
        exit 0
    fi
    NEW_HASH="$(git rev-parse --short=7 HEAD)"
fi

if [[ ! "${NEW_HASH}" =~ ^[0-9a-fA-F]{7}$ ]]; then
    echo "[ERROR] New git hash must be exactly 7 hexadecimal characters: ${NEW_HASH}" >&2
    exit 2
fi

if [[ -z "${OLD_HASH}" ]]; then
    OLD_HASH="$(strings "${BINARY}" | grep -E '^[0-9a-fA-F]{7}$' | head -n 1 || true)"
fi

if [[ ! "${OLD_HASH}" =~ ^[0-9a-fA-F]{7}$ ]]; then
    echo "[ERROR] Could not detect old 7-character git hash in binary." >&2
    echo "[ERROR] Provide it explicitly: $0 ${BINARY} ${NEW_HASH} <old-git-hash>" >&2
    exit 2
fi

if [[ "${OLD_HASH}" == "${NEW_HASH}" ]]; then
    echo "[INFO] Binary already contains git hash ${NEW_HASH}"
    exit 0
fi

COUNT="$(grep -a -o "${OLD_HASH}" "${BINARY}" | wc -l | tr -d ' ')"
if [[ "${COUNT}" == "0" ]]; then
    echo "[ERROR] Old git hash ${OLD_HASH} was not found in ${BINARY}" >&2
    exit 2
fi

perl -0pi -e "s/${OLD_HASH}/${NEW_HASH}/g" "${BINARY}"

if ! grep -a -q "${NEW_HASH}" "${BINARY}"; then
    echo "[ERROR] Failed to patch git hash in ${BINARY}" >&2
    exit 2
fi

echo "[INFO] Patched ${BINARY}: ${OLD_HASH} -> ${NEW_HASH} (${COUNT} occurrence(s))"
