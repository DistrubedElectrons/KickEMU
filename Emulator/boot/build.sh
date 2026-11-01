#!/usr/bin/env bash
#
# Build the KICK kernel, GPU kernel module and boot chain (SPL, ATF BL31, U-Boot,
# imx-boot) in a container, from the exact sources and configuration referenced by
# Linux/buildroot/configs/KTKick_defconfig.
#
# Usage: Emulator/boot/build.sh [options] [stage...]
#
# Stages (default: all, in this order):
#   kernel    linux-imx + board patches + Linux-defconfig -> Image, DTBs, modules
#   gpu       galcore.ko (kernel-module-imx-gpu-viv) against that kernel
#   atf       imx-atf BL31 for imx8mm
#   uboot     uboot-imx + board patches + defconfig -> SPL, u-boot-nodtb.bin, DTBs
#   imxboot   imx-mkimage + DDR training firmware -> imx-boot
#   dtdoc     (not in all) decompiled board DTS and the node table used by
#             Emulator/docs/board-dt.md
#
# Options:
#   --config <file>     configuration (default: Emulator/config/kickemu.json)
#   --out-dir <dir>     output directory (default: .boot.out_dir, relative paths resolve
#                       against the main checkout of this repository)
#   --ddr-fw <dir>      directory with the NXP LPDDR4/DDR4 training binaries
#                       (overrides .boot.ddr_firmware_dir)
#   --jobs <n>          parallel jobs inside the container (default: .boot.builder.jobs,
#                       0 means all CPUs)
#   --shell             open a shell in the builder instead of building
#   --drop-work         delete the build volume (sources and objects) and exit
#   -h, --help          show this help

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
LOG_TAG="boot"

usage() { sed -n '3,/^$/p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; }

OUT_DIR_ARG=""
DDR_FW_ARG=""
JOBS_ARG=""
SHELL_MODE=0
DROP_WORK=0
STAGES=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --config)    KICKEMU_CONFIG="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"; shift 2 ;;
        --out-dir)   OUT_DIR_ARG="$2"; shift 2 ;;
        --ddr-fw)    DDR_FW_ARG="$2"; shift 2 ;;
        --jobs)      JOBS_ARG="$2"; shift 2 ;;
        --shell)     SHELL_MODE=1; shift ;;
        --drop-work) DROP_WORK=1; shift ;;
        -h|--help)   usage; exit 0 ;;
        kernel|gpu|atf|uboot|imxboot|dtdoc|all) STAGES+=("$1"); shift ;;
        *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
    esac
done

# shellcheck source=lib/common.sh
source "${SCRIPT_DIR}/lib/common.sh"
# shellcheck source=lib/docker.sh
source "${SCRIPT_DIR}/lib/docker.sh"

ensure_docker

if [[ ${DROP_WORK} -eq 1 ]]; then
    log "removing build volume $(builder_volume)"
    docker volume rm "$(builder_volume)"
    exit 0
fi

OUT_DIR="$(out_path "${OUT_DIR_ARG:-$(cfg '.boot.out_dir')}")"
JOBS="${JOBS_ARG:-$(cfg '.boot.builder.jobs' 0)}"
[[ ${#STAGES[@]} -eq 0 ]] && STAGES=(all)

# The DDR firmware is NXP licensed. It is only used when the user points us at a copy.
DDR_FW="${DDR_FW_ARG:-$(cfg '.boot.ddr_firmware_dir' '')}"
DOCKER_EXTRA=()
if [[ -n "${DDR_FW}" ]]; then
    DDR_FW="$(cd "$(repo_path "${DDR_FW}")" 2>/dev/null && pwd)" \
        || die "DDR firmware directory not found: ${DDR_FW_ARG:-$(cfg '.boot.ddr_firmware_dir')}"
    DOCKER_EXTRA+=(-v "${DDR_FW}:/ddr-fw:ro" -e DDR_FW_DIR=/ddr-fw)
fi

build_builder_image
docker volume inspect "$(builder_volume)" >/dev/null 2>&1 || docker volume create "$(builder_volume)" >/dev/null

log "output directory: ${OUT_DIR}"
if [[ ${SHELL_MODE} -eq 1 ]]; then
    run_builder "${OUT_DIR}" "${DOCKER_EXTRA[@]+"${DOCKER_EXTRA[@]}"}" -- bash
else
    run_builder "${OUT_DIR}" "${DOCKER_EXTRA[@]+"${DOCKER_EXTRA[@]}"}" -- \
        bash /repo/Emulator/boot/container/build.sh "${STAGES[@]}"
fi
