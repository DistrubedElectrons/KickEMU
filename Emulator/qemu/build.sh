#!/usr/bin/env bash
#
# Build QEMU with the KICK machine and device models.
#
# The upstream QEMU tree is the submodule Emulator/third_party/qemu. Our sources live in
# Emulator/qemu/hw/kick and are linked into the submodule as hw/kick; integration hooks
# are applied from Emulator/qemu/patches. The submodule is never committed with changes.
#
# Usage: Emulator/qemu/build.sh [--clean] [--debug] [extra configure args...]
# Environment overrides:
#   KICKEMU_QEMU_BUILD_DIR   build directory      (default: Emulator/out/qemu-build)
#   KICKEMU_QEMU_PREFIX      install prefix       (default: Emulator/out/qemu)
#   KICKEMU_JOBS             parallel jobs        (default: number of CPUs)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
EMU_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"
QEMU_SRC="${EMU_DIR}/third_party/qemu"
BUILD_DIR="${KICKEMU_QEMU_BUILD_DIR:-${EMU_DIR}/out/qemu-build}"
PREFIX="${KICKEMU_QEMU_PREFIX:-${EMU_DIR}/out/qemu}"
JOBS="${KICKEMU_JOBS:-$(getconf _NPROCESSORS_ONLN)}"

CLEAN=0
DEBUG=0
EXTRA_ARGS=()
for arg in "$@"; do
    case "${arg}" in
        --clean) CLEAN=1 ;;
        --debug) DEBUG=1 ;;
        *) EXTRA_ARGS+=("${arg}") ;;
    esac
done

if [[ ! -f "${QEMU_SRC}/configure" ]]; then
    echo "QEMU submodule missing, run: git submodule update --init Emulator/third_party/qemu" >&2
    exit 1
fi

# Link our model directory into the QEMU tree and keep it out of the submodule status.
link="${QEMU_SRC}/hw/kick"
if [[ ! -L "${link}" ]]; then
    rm -rf "${link}"
    ln -s "${SCRIPT_DIR}/hw/kick" "${link}"
fi
tests_link="${QEMU_SRC}/tests/qtest/kick"
if [[ ! -L "${tests_link}" ]]; then
    rm -rf "${tests_link}"
    ln -s "${SCRIPT_DIR}/tests" "${tests_link}"
fi
# QEMU's qtest meson resolves "<name>.c" inside tests/qtest, so each of our test sources
# (named kick-*-test.c) is linked there individually. Stale links are removed first.
find "${QEMU_SRC}/tests/qtest" -maxdepth 1 -name 'kick-*.c' -type l -delete
for src in "${SCRIPT_DIR}"/tests/kick-*.c; do
    [[ -e "${src}" ]] || continue
    ln -s "${src}" "${QEMU_SRC}/tests/qtest/$(basename "${src}")"
done
exclude="$(git -C "${QEMU_SRC}" rev-parse --git-path info/exclude)"
case "${exclude}" in /*) ;; *) exclude="${QEMU_SRC}/${exclude}" ;; esac
mkdir -p "$(dirname "${exclude}")"
for p in /hw/kick /tests/qtest/kick '/tests/qtest/kick-*.c'; do
    grep -qxF "${p}" "${exclude}" 2>/dev/null || echo "${p}" >> "${exclude}"
done

# Apply integration patches once. A patch that reverse-applies cleanly is already in.
for patch in "${SCRIPT_DIR}"/patches/*.patch; do
    [[ -e "${patch}" ]] || continue
    if git -C "${QEMU_SRC}" apply --reverse --check "${patch}" 2>/dev/null; then
        continue
    fi
    echo "Applying $(basename "${patch}")"
    git -C "${QEMU_SRC}" apply "${patch}"
done

if [[ "${CLEAN}" == 1 ]]; then
    rm -rf "${BUILD_DIR}"
fi

ACCEL_ARGS=()
case "$(uname -s)" in
    Darwin) ACCEL_ARGS+=(--enable-hvf --enable-cocoa --audio-drv-list=coreaudio) ;;
    Linux)  ACCEL_ARGS+=(--enable-kvm) ;;
esac

DEBUG_ARGS=()
if [[ "${DEBUG}" == 1 ]]; then
    DEBUG_ARGS+=(--enable-debug)
fi

mkdir -p "${BUILD_DIR}"
if [[ ! -f "${BUILD_DIR}/build.ninja" ]]; then
    (cd "${BUILD_DIR}" && "${QEMU_SRC}/configure" \
        --prefix="${PREFIX}" \
        --target-list=aarch64-softmmu \
        --enable-tcg \
        --enable-vnc \
        --enable-slirp \
        --disable-docs \
        --disable-werror \
        "${ACCEL_ARGS[@]}" \
        ${DEBUG_ARGS[@]+"${DEBUG_ARGS[@]}"} \
        ${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"})
fi

ninja -C "${BUILD_DIR}" -j "${JOBS}"
ninja -C "${BUILD_DIR}" install >/dev/null
echo "QEMU installed to ${PREFIX}"
