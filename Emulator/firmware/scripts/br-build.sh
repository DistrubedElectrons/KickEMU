#!/usr/bin/env bash
#
# Builds the emulator variant of KTKick_defconfig with the repository's Buildroot 2021.02.3.
# Runs inside a debian:bullseye container (started by .github/workflows/firmware-image.yml):
# the 2021.02 host packages do not build with current host compilers.
#
# Mounts:  /src   repository checkout (read only)
#          /work  build directory (Buildroot copy, output, export)
#          /dl    Buildroot download directory (cached between runs)
# Env:     HOST_UID, HOST_GID  owner of the files left in /work/export and /dl
#          JOBS                make jobs inside packages (default: CPUs + 1)
#          STAGE               "config" stops after the configuration check
#
# Result in /work/export: rootfs.ext4.zst, Image, imx8mm-var-som-symphony.dtb,
# sdk.tar.gz (toolchain + sysroot, relocatable), buildroot.config, build.log.

set -euo pipefail

log() { echo "[br-build] $*" >&2; }

SRC=/src
WORK=/work
FW="${SRC}/Emulator/firmware"
BR="${WORK}/buildroot"
OUT="${WORK}/output"
EXPORT="${WORK}/export"
export BR2_DL_DIR=/dl
export FORCE_UNSAFE_CONFIGURE=1   # the container runs as root; some configure scripts refuse
JOBS="${JOBS:-$(( $(nproc) + 1 ))}"

# --- host packages ---------------------------------------------------------------------------
export DEBIAN_FRONTEND=noninteractive
if ! apt-get update -qq; then
    log "deb.debian.org has no bullseye any more, using archive.debian.org"
    cat > /etc/apt/sources.list <<'EOF'
deb http://archive.debian.org/debian bullseye main
deb http://archive.debian.org/debian-security bullseye-security main
EOF
    apt-get -o Acquire::Check-Valid-Until=false update -qq
fi
apt-get install -y -qq --no-install-recommends \
    build-essential bash patch gzip bzip2 perl tar cpio unzip rsync file bc wget \
    python3 git ca-certificates xz-utils zstd debianutils >/dev/null

# Package sources that moved: NXP left source.codeaurora.org for GitHub, and GitHub no longer
# serves the git:// protocol. Buildroot clones these with git, so the rewrite applies.
git config --global url."https://github.com/nxp-imx/".insteadOf "https://source.codeaurora.org/external/imx/"
git config --global url."https://github.com/".insteadOf "git://github.com/"

# --- Buildroot tree --------------------------------------------------------------------------
# The repository's copy of Buildroot lost the executable bits of its scripts. Work on a copy
# and restore them for every file that starts with a shebang (the board rootfs overlay keeps
# its modes: that is the KICK's content).
log "copying Linux/buildroot"
mkdir -p "${BR}" "${OUT}" "${EXPORT}"
rsync -a --delete "${SRC}/Linux/buildroot/" "${BR}/"
find "${BR}" -type f -not -path '*/rootfs_overlay/*' -print0 \
    | xargs -0 grep -lI -m1 '^#!' 2>/dev/null \
    | while IFS= read -r f; do
          if [[ "$(head -c 2 "${f}")" == '#!' ]]; then chmod +x "${f}"; fi
      done

# --- configuration ---------------------------------------------------------------------------
make -C "${BR}" O="${OUT}" KTKick_defconfig >/dev/null
sed -e "s|@FIRMWARE_DIR@|${FW}|g" "${FW}/buildroot/emulator.fragment" > "${WORK}/emulator.fragment"
cat "${WORK}/emulator.fragment" >> "${OUT}/.config"
make -C "${BR}" O="${OUT}" olddefconfig >/dev/null
cp "${OUT}/.config" "${EXPORT}/buildroot.config"
python3 "${FW}/buildroot/check-config.py" \
    "${BR}/configs/KTKick_defconfig" "${WORK}/emulator.fragment" "${OUT}/.config"
[[ "${STAGE:-}" == config ]] && exit 0

# --- downloads -------------------------------------------------------------------------------
# The kernel is a custom git version: Buildroot would clone all of linux-imx to get one
# commit. GitHub's archive of that commit is the same tree (Buildroot strips the top-level
# directory and does not check hashes of custom kernel versions).
eval "$(make -s -C "${BR}" O="${OUT}" printvars VARS='LINUX_SOURCE LINUX_DL_DIR' 2>/dev/null \
        | sed -n 's/^\(LINUX_SOURCE\|LINUX_DL_DIR\)=\(.*\)$/\1="\2"/p')"
kurl="$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_REPO_URL="\(.*\)"$/\1/p' "${OUT}/.config")"
kver="$(sed -n 's/^BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION="\(.*\)"$/\1/p' "${OUT}/.config")"
if [[ -n "${kurl}" && -n "${LINUX_SOURCE:-}" && ! -s "${LINUX_DL_DIR}/${LINUX_SOURCE}" ]]; then
    log "fetching ${kurl%.git} ${kver} as ${LINUX_SOURCE}"
    mkdir -p "${LINUX_DL_DIR}"
    wget -q -O "${LINUX_DL_DIR}/.${LINUX_SOURCE}" "${kurl%.git}/archive/${kver}.tar.gz"
    mv "${LINUX_DL_DIR}/.${LINUX_SOURCE}" "${LINUX_DL_DIR}/${LINUX_SOURCE}"
fi

log "downloading sources"
if ! make -C "${BR}" O="${OUT}" source > "${EXPORT}/source.log" 2>&1; then
    tail -n 60 "${EXPORT}/source.log"
    exit 1
fi

# --- build -----------------------------------------------------------------------------------
log "building (BR2_JLEVEL=${JOBS}); log in build.log"
start=$(date +%s)
if ! make -C "${BR}" O="${OUT}" BR2_JLEVEL="${JOBS}" > "${EXPORT}/build.log" 2>&1; then
    tail -n 150 "${EXPORT}/build.log"
    exit 1
fi
log "build took $(( ($(date +%s) - start) / 60 )) min"

log "building the SDK"
make -C "${BR}" O="${OUT}" sdk >> "${EXPORT}/build.log" 2>&1

# --- export ----------------------------------------------------------------------------------
img="${OUT}/images"
kimage="$(ls -d "${OUT}"/build/linux-*/arch/arm64/boot/Image | head -n 1)"
cp "${kimage}" "${EXPORT}/Image"
cp "${img}/imx8mm-var-som-symphony.dtb" "${EXPORT}/"
zstd -q -T0 -10 < "${img}/rootfs.ext4" > "${EXPORT}/rootfs.ext4.zst"
cp "${img}"/*_sdk-buildroot.tar.gz "${EXPORT}/sdk.tar.gz"
ls -l "${EXPORT}" >&2
du -sh "${OUT}" "${BR2_DL_DIR}" >&2 || true

chown -R "${HOST_UID:-0}:${HOST_GID:-0}" "${EXPORT}" "${BR2_DL_DIR}"
