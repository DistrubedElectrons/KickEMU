#!/usr/bin/env bash
#
# Inside the builder: render the board's eMMC genimage configuration and run genimage.
# Started by Emulator/boot/mkimage-emmc.sh. Inputs are mounted at /in/rootfs.img and
# /in/imx-boot, the output directory at /out.
#
# The configuration is rendered with the same substitutions as the board's post-image.sh
# (%IMXOFFSET%, %BR2_TARGET_ROOTFS_EXT2_SIZE%). Two changes turn the partition-table-only
# image Buildroot generates into a bootable disk: the device size comes from
# the configuration, and the rootfs partition gets the given rootfs image as content.
# The storage partition stays empty; the KICK formats it on first boot
# (kick-storage-mkfs.service).

set -euo pipefail

: "${REPO_ROOT:=/repo}"
LOG_TAG="emmc"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/common.sh
source "${HERE}/../lib/common.sh"
# shellcheck source=lib.sh
source "${HERE}/lib.sh"

: "${EMMC_SIZE:?}" "${EMMC_NAME:?}"
cfg_template="$(repo_path "$(cfg '.boot.emmc.genimage_cfg')")"
post_image="$(br_path BR2_ROOTFS_POST_IMAGE_SCRIPT)"

# post-image.sh is not sourceable (it runs main at the end), so its pure imx_offset()
# helper is evaluated on its own. BR2_CONFIG is what that helper greps.
for s in $(br BR2_ROOTFS_POST_IMAGE_SCRIPT); do
    if grep -q '^imx_offset()' "${BR_DIR}/${s}"; then post_image="${BR_DIR}/${s}"; fi
done
eval "$(sed -n '/^imx_offset()/,/^}/p' "${post_image}")"
BR2_CONFIG="${DEFCONFIG}"
imx_offset_value="$(imx_offset)"
rootfs_size="${ROOTFS_SIZE:-$(br BR2_TARGET_ROOTFS_EXT2_SIZE)}"

# The boot image name genimage expects is the one written in the template.
boot_name="$(sed -n 's/^[[:space:]]*image = "\([^"]*\)".*/\1/p' "${cfg_template}" | head -n 1)"
[[ -n "${boot_name}" ]] || die "no boot image name in ${cfg_template}"

stage="${WORK}/build/emmc"
rm -rf "${stage}"
mkdir -p "${stage}/input" "${stage}/output" "${stage}/root"
cp /in/imx-boot "${stage}/input/${boot_name}"
cp /in/rootfs.img "${stage}/input/rootfs.ext4"

# Render: placeholders as in post-image.sh, then the disk size (first "size =" line,
# which belongs to the image block) and the rootfs partition content.
sed -e "s/%IMXOFFSET%/${imx_offset_value}/" \
    -e "s/%BR2_TARGET_ROOTFS_EXT2_SIZE%/${rootfs_size}/" \
    -e "0,/^\([[:space:]]*\)size = .*/s//\1size = ${EMMC_SIZE}/" \
    -e '/^[[:space:]]*partition rootfs[[:space:]]*{/a\	image = "rootfs.ext4"' \
    "${cfg_template}" > "${stage}/genimage.cfg"
image_name="$(sed -n 's/^image \([^ {]*\).*/\1/p' "${stage}/genimage.cfg" | head -n 1)"
log "layout: imx-boot at ${imx_offset_value}, rootfs ${rootfs_size}, device ${EMMC_SIZE}"

genimage \
    --rootpath "${stage}/root" \
    --tmppath "${stage}/tmp" \
    --inputpath "${stage}/input" \
    --outputpath "${stage}/output" \
    --config "${stage}/genimage.cfg"

# Copy out keeping holes, so the multi-GB image only uses the space of its content.
cp --sparse=always "${stage}/output/${image_name}" "${OUT}/.${EMMC_NAME}.tmp"
mv -f "${OUT}/.${EMMC_NAME}.tmp" "${OUT}/${EMMC_NAME}"
sgdisk -p "${OUT}/${EMMC_NAME}" >&2
sgdisk -i 2 "${OUT}/${EMMC_NAME}" | grep -i 'unique guid' >&2
rm -rf "${stage}"
log "wrote ${EMMC_NAME} ($(du -h "${OUT}/${EMMC_NAME}" | cut -f1) on disk, ${EMMC_SIZE} apparent)"
