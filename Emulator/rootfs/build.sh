#!/usr/bin/env bash
#
# Build the Debian 12 root file system that runs the KICK application (KT_Kick) on the
# emulated KICK: Debian packages installed in a debian:bookworm container, the vendor kernel
# modules, KT_Kick with its /app data, X on the framebuffer, and the KICK service files from
# the Buildroot board overlay. The container is exported with "docker export" and packed into
# an ext4 image with mke2fs -d; the image then goes into rootfs partition 1 of an eMMC image
# with the real KICK GPT (Emulator/boot/mkimage-emmc.sh).
#
# Usage: Emulator/rootfs/build.sh [options] [step...]
#
# Steps (default: app rootfs emmc):
#   app       fetch or build the KT_Kick package (.rootfs.app.source: artifact, source, file)
#   rootfs    install the packages and pack rootfs.ext4
#   emmc      write the eMMC image (.rootfs.emmc_image) with rootfs.ext4 in partition 1
#
# Options:
#   --config <file>     configuration (default: Emulator/config/kickemu.json)
#   --boot-dir <dir>    boot build outputs: kernel modules, galcore, imx-boot
#                       (default: .boot.out_dir)
#   --out-dir <dir>     output directory (default: .rootfs.out_dir)
#   --emmc-image <file> eMMC image to write (default: .rootfs.emmc_image)
#   --app-source <src>  artifact, source or file (default: .rootfs.app.source)
#   --package <file>    KT_Kick-linux-arm64.tar.gz to use (implies --app-source file)
#   -h, --help          show this help
#
# Relative paths in the configuration resolve against the main checkout.
# Requires Emulator/boot/build.sh kernel gpu imxboot to have run first.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_TAG="rootfs"

usage() { sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }
abspath() { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd)" "$(basename "$1")"); }

BOOT_ARG="" OUT_ARG="" EMMC_ARG="" SOURCE_ARG="" PACKAGE_ARG=""
STEPS=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)     KICKEMU_CONFIG="$(abspath "$2")"; shift 2 ;;
        --boot-dir)   BOOT_ARG="$2"; shift 2 ;;
        --out-dir)    OUT_ARG="$2"; shift 2 ;;
        --emmc-image) EMMC_ARG="$2"; shift 2 ;;
        --app-source) SOURCE_ARG="$2"; shift 2 ;;
        --package)    PACKAGE_ARG="$2"; SOURCE_ARG="file"; shift 2 ;;
        -h|--help)    usage; exit 0 ;;
        app|rootfs|emmc) STEPS+=("$1"); shift ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done
[[ ${#STEPS[@]} -gt 0 ]] || STEPS=(app rootfs emmc)

# shellcheck source=../boot/lib/common.sh
source "${REPO_ROOT}/Emulator/boot/lib/common.sh"
# shellcheck source=../boot/lib/docker.sh
source "${REPO_ROOT}/Emulator/boot/lib/docker.sh"
# shellcheck source=lib/app.sh
source "${SCRIPT_DIR}/lib/app.sh"
# shellcheck source=lib/stage.sh
source "${SCRIPT_DIR}/lib/stage.sh"

BOOT_OUT="$(out_path "${BOOT_ARG:-$(cfg '.boot.out_dir')}")"
OUT_DIR="$(out_path "${OUT_ARG:-$(cfg '.rootfs.out_dir')}")"
EMMC_IMAGE="$(out_path "${EMMC_ARG:-$(cfg '.rootfs.emmc_image')}")"
APP_SOURCE="${SOURCE_ARG:-$(cfg '.rootfs.app.source')}"
PLATFORM="$(cfg '.rootfs.platform')"
APP_PACKAGE="${OUT_DIR}/app/$(cfg '.rootfs.app.artifact').tar.gz"
ROOTFS_IMAGE="${OUT_DIR}/rootfs.ext4"

ensure_docker
mkdir -p "${OUT_DIR}"

for step in "${STEPS[@]}"; do
    case "${step}" in
        app)
            case "${APP_SOURCE}" in
                artifact) app_from_artifact ;;
                source)   app_from_source ;;
                file)     app_from_file "${PACKAGE_ARG:-$(cfg '.rootfs.app.package_file' '')}" ;;
                *) die "unknown app source ${APP_SOURCE} (artifact, source, file)" ;;
            esac
            ;;
        rootfs)
            stage_rootfs
            pack_rootfs
            ;;
        emmc)
            [[ -f "${ROOTFS_IMAGE}" ]] || die "no ${ROOTFS_IMAGE}; run the rootfs step first"
            "${REPO_ROOT}/Emulator/boot/mkimage-emmc.sh" \
                ${KICKEMU_CONFIG:+--config "${KICKEMU_CONFIG}"} \
                --rootfs "${ROOTFS_IMAGE}" \
                --imx-boot "${BOOT_OUT}/imx-boot" \
                --output "${EMMC_IMAGE}" \
                --rootfs-size "$(cfg '.rootfs.image_size')"
            ;;
    esac
done
