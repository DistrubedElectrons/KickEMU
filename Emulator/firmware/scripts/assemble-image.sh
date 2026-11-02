#!/usr/bin/env bash
#
# Puts KT_Kick into the Buildroot root file system and writes the eMMC image with the KICK
# partition layout. Runs on the GitHub runner (needs sudo for the loop mount).
#
# Usage: assemble-image.sh <br export dir> <KT_Kick.tar.gz> <output dir>
#
# <br export dir> is what br-build.sh exported (rootfs.ext4.zst, Image, the DTB, sdk.tar.gz).
# The eMMC layout comes from the board's genimage.cfg.emmc.partiton.table, as in
# Emulator/boot/container/emmc.sh: GPT at 7M with the KICK disk UUID, rootfs in partition 1
# (PARTUUID 70f29e13-...) at 8M, storage partition 2 (PARTUUID 56d2e551-...) filling the
# rest. The imx-boot entry is dropped: QEMU loads the kernel directly and imx-boot needs the
# NXP DDR firmware. The image is 14 GiB (the KICK's eMMC), sparse, and is written
# zstd-compressed.
#
# Output: kick-emmc.img.zst, Image, imx8mm-var-som-symphony.dtb, manifest.txt, SHA256SUMS.

set -euo pipefail

log() { echo "[assemble] $*" >&2; }

[[ $# -eq 3 ]] || { sed -n '3,16p' "$0" >&2; exit 2; }
BR_EXPORT="$(realpath "$1")"
APP_TAR="$(realpath "$2")"
OUT="$(realpath -m "$3")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "${HERE}/../../.." && pwd)"
EMMC_SIZE=14G
DTB=imx8mm-var-som-symphony.dtb

work="$(mktemp -d)"
cleanup() {
    if mountpoint -q "${work}/mnt" 2>/dev/null; then sudo umount "${work}/mnt"; fi
    sudo rm -rf "${work}"
}
trap cleanup EXIT
mkdir -p "${OUT}" "${work}/mnt" "${work}/app" "${work}/sdk"

# --- root file system with KT_Kick ---------------------------------------------------------------
zstd -q -d "${BR_EXPORT}/rootfs.ext4.zst" -o "${work}/rootfs.ext4"
tar -xzf "${APP_TAR}" -C "${work}/app"
app="${work}/app/KT_Kick"
sudo mount -o loop "${work}/rootfs.ext4" "${work}/mnt"
root="${work}/mnt"
[[ -f "${root}/usr/bin/kt_kick.sh" ]] || { echo "rootfs has no kt_kick.sh" >&2; exit 1; }
sudo install -m 0755 -o 0 -g 0 "${app}/KT_Kick" "${root}/usr/bin/KT_Kick"
sudo mkdir -p "${root}/app"
sudo cp -R "${app}/assets" "${app}/templates" "${app}/demos" "${root}/app/"
sudo install -m 0644 -o 0 -g 0 "${app}/manifest.json" "${root}/app/manifest.json"
sudo chown -R 0:0 "${root}/app"
# Libraries resolved against the image itself, not only the SDK sysroot.
missing=0
while IFS= read -r lib; do
    if [[ -z "$(sudo find "${root}/lib" "${root}/usr/lib" -maxdepth 1 -name "${lib}" -print -quit)" ]]; then
        echo "missing in the image: ${lib}" >&2
        missing=1
    fi
done < "${app}/shared-libraries.txt"
df -h "${root}" >&2
sudo umount "${root}"
[[ "${missing}" -eq 0 ]] || exit 1
/sbin/e2fsck -fn "${work}/rootfs.ext4" >&2

# --- eMMC image --------------------------------------------------------------------------------
tar -xzf "${BR_EXPORT}/sdk.tar.gz" -C "${work}/sdk" --strip-components=1
genimage="${work}/sdk/bin/genimage"
cfg_template="${REPO}/Linux/buildroot/board/freescale/imx8mm_var_som/genimage.cfg.emmc.partiton.table"
rootfs_size="$(sed -n 's/^BR2_TARGET_ROOTFS_EXT2_SIZE="\(.*\)"$/\1/p' "${BR_EXPORT}/buildroot.config")"
[[ -n "${rootfs_size}" ]] || { echo "no BR2_TARGET_ROOTFS_EXT2_SIZE" >&2; exit 1; }

# Render the board template: drop the imx-boot partition block, set the device size (first
# "size =" line, the image block) and the rootfs size, and give the rootfs partition content.
python3 - "${cfg_template}" "${work}/genimage.cfg" "${EMMC_SIZE}" "${rootfs_size}" <<'PY'
import re, sys
src, dst, emmc_size, rootfs_size = sys.argv[1:5]
text = open(src).read()
text, n = re.subn(r"\n[ \t]*partition imx-boot[ \t]*\{.*?\n[ \t]*\}[ \t]*\n", "\n", text, flags=re.S)
assert n == 1, "imx-boot partition block not found"
text, n = re.subn(r"(?m)^([ \t]*)size = .*$", lambda m: f"{m.group(1)}size = {emmc_size}", text, count=1)
assert n == 1
text = text.replace("%BR2_TARGET_ROOTFS_EXT2_SIZE%", rootfs_size)
text, n = re.subn(r"(?m)^([ \t]*partition rootfs[ \t]*\{)$", r'\1\n\timage = "rootfs.ext4"', text)
assert n == 1, "rootfs partition not found"
assert "%" not in text, "unrendered placeholder"
open(dst, "w").write(text)
PY
cat "${work}/genimage.cfg" >&2
mkdir -p "${work}/gi-input" "${work}/gi-output" "${work}/gi-root"
mv "${work}/rootfs.ext4" "${work}/gi-input/rootfs.ext4"
"${genimage}" --rootpath "${work}/gi-root" --tmppath "${work}/gi-tmp" \
    --inputpath "${work}/gi-input" --outputpath "${work}/gi-output" \
    --config "${work}/genimage.cfg"
image_name="$(sed -n 's/^image \([^ {]*\).*/\1/p' "${work}/genimage.cfg" | head -n 1)"
img="${work}/kick-emmc.img"
# genimage stops after the last written data (empty storage partition, no backup GPT).
dd if="${work}/gi-output/${image_name}" of="${img}" bs=1M conv=sparse status=none
truncate -s "${EMMC_SIZE}" "${img}"
sgdisk -p "${img}" >&2
sgdisk -i 1 "${img}" | grep -i 'unique guid' >&2
sgdisk -i 2 "${img}" | grep -i 'unique guid' >&2

# --- outputs -----------------------------------------------------------------------------------
img_sha="$(sha256sum "${img}" | cut -d' ' -f1)"
zstd -q -T0 -12 "${img}" -o "${OUT}/kick-emmc.img.zst"
cp "${BR_EXPORT}/Image" "${BR_EXPORT}/${DTB}" "${OUT}/"
{
    echo "KICK emulator firmware image (Emulator/firmware)"
    echo "repository commit: ${GITHUB_SHA:-$(git -C "${REPO}" rev-parse HEAD 2>/dev/null || echo unknown)}"
    echo "kick-emmc.img: ${EMMC_SIZE} raw eMMC image (zstd), sha256 after decompression ${img_sha}"
    echo "kernel: $(strings "${OUT}/Image" | grep -m1 '^Linux version' || true)"
    echo "KT_Kick:"
    sed 's/^/  /' "${app}/manifest.json"
} > "${OUT}/manifest.txt"
(cd "${OUT}" && sha256sum Image "${DTB}" kick-emmc.img.zst manifest.txt > SHA256SUMS)
ls -l "${OUT}" >&2
cat "${OUT}/SHA256SUMS" >&2
