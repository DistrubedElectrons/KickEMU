#!/usr/bin/env bash
#
# Build an Alpine Linux desktop (Xfce on the 1024x600 panel, touch, USB keyboard and
# mouse) for the emulated KICK, as a proof that a general-purpose distribution runs
# on the machine. Output: Emulator/out/desktop/emmc-desktop.img; boot it with
# `KickEmu --profile alpine-desktop`.
#
# Usage: Emulator/desktop/build.sh [options]
#
# Options:
#   --config <file>     configuration (default: Emulator/config/kickemu.json)
#   -h, --help          show this help
#
# Requires the vendor kernel build (Emulator/boot/build.sh kernel) and a container
# runtime (OrbStack or Docker). Runs the Alpine container as linux/arm64.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_TAG="desktop"

usage() { sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

while [[ $# -gt 0 ]]; do
    case "$1" in
        --config) KICKEMU_CONFIG="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# shellcheck source=../boot/lib/common.sh
source "${REPO_ROOT}/Emulator/boot/lib/common.sh"
# shellcheck source=../boot/lib/docker.sh
source "${REPO_ROOT}/Emulator/boot/lib/docker.sh"

BOOT_OUT="$(out_path "$(cfg '.boot.out_dir')")"
OUT_DIR="$(out_path "$(cfg '.desktop.out_dir')")"
[[ -f "${BOOT_OUT}/kernel.release" ]] || die "no kernel build in ${BOOT_OUT}; run Emulator/boot/build.sh kernel"
KERNEL_RELEASE="$(cat "${BOOT_OUT}/kernel.release")"

ensure_docker
mkdir -p "${OUT_DIR}"
log "Alpine $(cfg '.desktop.base_image') with Xfce, kernel ${KERNEL_RELEASE}, output ${OUT_DIR}"
docker run --rm \
    --platform linux/arm64 \
    -v "${REPO_ROOT}:/repo:ro" \
    -v "${BOOT_OUT}:/boot-out:ro" \
    -v "${OUT_DIR}:/out" \
    -e "PACKAGES=$(cfg_list '.desktop.packages' | tr '\n' ' ')" \
    -e "KERNEL_RELEASE=${KERNEL_RELEASE}" \
    -e "ROOTFS_SIZE=$(cfg '.desktop.rootfs_size')" \
    -e "EMMC_SIZE=$(cfg '.desktop.emmc_size')" \
    -e "ROOT_PASSWORD=$(cfg '.desktop.root_password')" \
    -e "HOSTNAME_=$(cfg '.desktop.hostname')" \
    "$(cfg '.desktop.base_image')" sh /repo/Emulator/desktop/container/mkdesktop.sh
