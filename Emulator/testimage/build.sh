#!/usr/bin/env bash
#
# Build the KICK hardware test image: a small aarch64 root file system (Alpine packages,
# static busybox, test tools, the kernel modules and galcore from the boot build, and
# kick-selftest), as initramfs.cpio.gz and rootfs.ext4.
#
# Usage: Emulator/testimage/build.sh [options]
#
# Options:
#   --config <file>     configuration (default: Emulator/config/kickemu.json)
#   --boot-dir <dir>    boot build outputs (default: .boot.out_dir)
#   --out-dir <dir>     output directory (default: .testimage.out_dir)
#   -h, --help          show this help
#
# Relative paths in the configuration resolve against the main checkout.
# Requires Emulator/boot/build.sh kernel gpu to have run first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_TAG="testimage"

usage() { sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

BOOT_ARG="" OUT_ARG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)   KICKEMU_CONFIG="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        --boot-dir) BOOT_ARG="$2"; shift 2 ;;
        --out-dir)  OUT_ARG="$2"; shift 2 ;;
        -h|--help)  usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# shellcheck source=../boot/lib/common.sh
source "${REPO_ROOT}/Emulator/boot/lib/common.sh"
# shellcheck source=../boot/lib/docker.sh
source "${REPO_ROOT}/Emulator/boot/lib/docker.sh"

BOOT_OUT="$(out_path "${BOOT_ARG:-$(cfg '.boot.out_dir')}")"
OUT_DIR="$(out_path "${OUT_ARG:-$(cfg '.testimage.out_dir')}")"
[[ -f "${BOOT_OUT}/kernel.release" ]] || die "no kernel build in ${BOOT_OUT}; run Emulator/boot/build.sh kernel gpu"
KERNEL_RELEASE="$(cat "${BOOT_OUT}/kernel.release")"
[[ -f "${BOOT_OUT}/modules-${KERNEL_RELEASE}.tar.gz" ]] || die "modules tarball for ${KERNEL_RELEASE} missing"
[[ -f "${BOOT_OUT}/galcore.ko" ]] || die "galcore.ko missing; run Emulator/boot/build.sh gpu"

PACKAGES="$(cfg_list '.testimage.packages' | tr '\n' ' ')"
[[ -n "${PACKAGES// /}" ]] || die "no packages in .testimage.packages"

ensure_docker
mkdir -p "${OUT_DIR}"
log "kernel ${KERNEL_RELEASE}, output ${OUT_DIR}"
docker run --rm \
    --platform "$(cfg '.testimage.platform')" \
    -v "${REPO_ROOT}:/repo:ro" \
    -v "${BOOT_OUT}:/boot-out:ro" \
    -v "${OUT_DIR}:/out" \
    -e "PACKAGES=${PACKAGES}" \
    -e "EXT4_SIZE=$(cfg '.testimage.rootfs_ext4_size')" \
    -e "KERNEL_RELEASE=${KERNEL_RELEASE}" \
    "$(cfg '.testimage.base_image')" sh /repo/Emulator/testimage/container/mkrootfs.sh

ls -l "${OUT_DIR}/initramfs.cpio.gz" "${OUT_DIR}/rootfs.ext4"
