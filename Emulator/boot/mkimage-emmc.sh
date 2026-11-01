#!/usr/bin/env bash
#
# Assemble a raw eMMC image with the real KICK layout, from the board's genimage
# configuration (genimage.cfg.emmc.partiton.table): GPT with the KICK disk UUID and
# partition UUIDs, imx-boot at the i.MX8MM boot offset, the rootfs partition and the
# KICK storage partition (PARTUUID 56d2e551-db06-46da-bb58-274c6ee725d2) filling the
# rest of the device. Runs genimage in the boot builder container.
#
# Usage: Emulator/boot/mkimage-emmc.sh [options]
#
# Options:
#   --config <file>       configuration (default: Emulator/config/kickemu.json)
#   --rootfs <file>       ext2/3/4 image for the rootfs partition
#                         (default: .boot.emmc.rootfs_image)
#   --imx-boot <file>     boot image (default: <.boot.out_dir>/imx-boot)
#   --output <file>       eMMC image (default: .storage.emmc_image)
#   --size <size>         eMMC size, genimage syntax (default: .storage.emmc_size)
#   --rootfs-size <size>  rootfs partition size (default: .boot.emmc.rootfs_size, or
#                         BR2_TARGET_ROOTFS_EXT2_SIZE from the defconfig when empty)
#   -h, --help            show this help
#
# Relative paths in the configuration resolve against the main checkout.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_TAG="emmc"

usage() { sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
abspath() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }

ROOTFS_ARG="" IMXBOOT_ARG="" OUTPUT_ARG="" SIZE_ARG="" ROOTFS_SIZE_ARG=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)      KICKEMU_CONFIG="$(abspath "$2")"; shift 2 ;;
        --rootfs)      ROOTFS_ARG="$2"; shift 2 ;;
        --imx-boot)    IMXBOOT_ARG="$2"; shift 2 ;;
        --output)      OUTPUT_ARG="$2"; shift 2 ;;
        --size)        SIZE_ARG="$2"; shift 2 ;;
        --rootfs-size) ROOTFS_SIZE_ARG="$2"; shift 2 ;;
        -h|--help)     usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
# shellcheck source=lib/docker.sh
source "${SCRIPT_DIR}/lib/docker.sh"

ROOTFS="$(out_path "${ROOTFS_ARG:-$(cfg '.boot.emmc.rootfs_image')}")"
IMXBOOT="$(out_path "${IMXBOOT_ARG:-$(cfg '.boot.out_dir')/imx-boot}")"
OUTPUT="$(out_path "${OUTPUT_ARG:-$(cfg '.storage.emmc_image')}")"
SIZE="${SIZE_ARG:-$(cfg '.storage.emmc_size')}"
ROOTFS_SIZE="${ROOTFS_SIZE_ARG:-$(cfg '.boot.emmc.rootfs_size' '')}"
JOBS=0

[[ -f "${ROOTFS}" ]]  || die "rootfs image not found: ${ROOTFS} (build Emulator/testimage first or pass --rootfs)"
[[ -f "${IMXBOOT}" ]] || die "imx-boot not found: ${IMXBOOT} (run Emulator/boot/build.sh first or pass --imx-boot)"
ROOTFS="$(abspath "${ROOTFS}")"
IMXBOOT="$(abspath "${IMXBOOT}")"
mkdir -p "$(dirname "${OUTPUT}")"

ensure_docker
build_builder_image
docker volume inspect "$(builder_volume)" >/dev/null 2>&1 || docker volume create "$(builder_volume)" >/dev/null

log "rootfs ${ROOTFS}"
log "imx-boot ${IMXBOOT}"
run_builder "$(dirname "${OUTPUT}")" \
    -v "${ROOTFS}:/in/rootfs.img:ro" \
    -v "${IMXBOOT}:/in/imx-boot:ro" \
    -e "EMMC_SIZE=${SIZE}" -e "ROOTFS_SIZE=${ROOTFS_SIZE}" \
    -e "EMMC_NAME=$(basename "${OUTPUT}")" -- \
    bash /repo/Emulator/boot/container/emmc.sh
log "eMMC image: ${OUTPUT}"
