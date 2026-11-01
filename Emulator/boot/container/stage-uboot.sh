# U-Boot stage: varigit uboot-imx at the pinned commit with the board patches and the
# board U-Boot configuration. Built in-tree like Buildroot does, target "all", which
# produces the SPL, u-boot-nodtb.bin, the U-Boot DTBs and tools/mkimage.
#
# shellcheck shell=bash

UBOOT_SRC="${WORK}/src/uboot"

stage_uboot() {
    local url rev
    url="$(rewrite_url "$(br BR2_TARGET_UBOOT_CUSTOM_REPO_URL)")"
    rev="$(br BR2_TARGET_UBOOT_CUSTOM_REPO_VERSION)"
    fetch_git uboot "${url}" "${rev}"
    apply_patches uboot "$(br_path BR2_TARGET_UBOOT_PATCH)"

    local uconfig
    uconfig="$(br_path BR2_TARGET_UBOOT_CUSTOM_CONFIG_FILE)"
    if ! cmp -s "${uconfig}" "${UBOOT_SRC}/.kickemu-defconfig"; then
        cp "${uconfig}" "${UBOOT_SRC}/.config"
        cp "${uconfig}" "${UBOOT_SRC}/.kickemu-defconfig"
        make -C "${UBOOT_SRC}" ARCH=arm CROSS_COMPILE= olddefconfig
    fi
    log "uboot: building with ${JOBS} jobs"
    make -C "${UBOOT_SRC}" -j"${JOBS}" ARCH=arm CROSS_COMPILE= all

    publish "${UBOOT_SRC}/spl/u-boot-spl.bin"
    publish "${UBOOT_SRC}/$(br BR2_TARGET_UBOOT_FORMAT_CUSTOM_NAME)"
    publish "${UBOOT_SRC}/u-boot.dtb"
    # ELF files with symbols, for debugging SPL and U-Boot under the emulator.
    publish "${UBOOT_SRC}/spl/u-boot-spl" u-boot-spl.elf
    publish "${UBOOT_SRC}/u-boot" u-boot.elf
    publish "${UBOOT_SRC}/.config" uboot.config
}
