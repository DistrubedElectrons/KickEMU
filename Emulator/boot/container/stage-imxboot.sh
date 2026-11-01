# imx-boot stage: builds imx-mkimage (Buildroot host package), prepares the DDR training
# firmware blob and runs the board's own bootloader post-image script
# (imx8-bootloader-prepare.sh) in a Buildroot-like environment, so imx-boot is assembled
# with exactly the commands, load addresses and DTB list the KICK build uses.
#
# DDR training firmware: NXP firmware-imx is under the NXP EULA and is never downloaded
# here. When DDR_FW_DIR points at the extracted firmware/ddr/synopsys files they are used.
# Otherwise zero-filled blobs of the same padded sizes are used and the result is valid
# for the emulator only (its DDR PHY model does not execute the training firmware).
#
# shellcheck shell=bash

# mkimage_host_tools <host bin dir>: builds mkimage_imx8 and installs the helper scripts
# under the names Buildroot uses (see package/imx-mkimage/imx-mkimage.mk).
mkimage_host_tools() {
    local hostbin="$1" pkg mk version site src="${WORK}/src/imx-mkimage"
    pkg="$(repo_path "$(cfg '.boot.packages.imx_mkimage')")"
    mk="${pkg}/imx-mkimage.mk"
    version="$(mk_get "${mk}" IMX_MKIMAGE_VERSION)"
    site="$(rewrite_url "$(mk_get "${mk}" IMX_MKIMAGE_SITE)")"
    fetch_git imx-mkimage "${site}" "${version}"
    apply_patches imx-mkimage "${pkg}"

    log "imxboot: building mkimage_imx8 (${version})"
    make -C "${src}/iMX8M" -f soc.mak CFLAGS="-O2 -std=c99" mkimage_imx8 >/dev/null
    mkdir -p "${hostbin}"
    install -m 755 "${src}/iMX8M/mkimage_imx8" "${hostbin}/mkimage_imx8"
    install -m 755 "${src}/iMX8M/mkimage_fit_atf.sh" "${hostbin}/mkimage_fit_atf.sh"
    install -m 755 "${src}/scripts/pad_image.sh" "${hostbin}/imx8m_pad_image.sh"
    # U-Boot's own mkimage from the pinned tree stands in for host-uboot-tools.
    install -m 755 "${UBOOT_SRC}/tools/mkimage" "${hostbin}/mkimage"
}

# ddr_fw_triples: "<imem> <dmem> <out>" for every FIRMWARE_IMX_PREPARE_DDR_FW call in the
# firmware-imx.mk block of the selected memory type, in the order the recipe uses them.
ddr_fw_triples() {
    local mk="$1" kind="$2"
    awk -v cond="ifeq (\$(BR2_PACKAGE_FIRMWARE_IMX_${kind}),y)" '
        index($0, cond) == 1 { inside = 1; next }
        inside && /^endif/   { exit }
        inside && match($0, /^[[:space:]]*[a-z0-9_]+,[a-z0-9_]+,[a-z0-9_]+\)/) {
            line = $0; gsub(/[[:space:])]/, "", line); gsub(/,/, " ", line); print line
        }
    ' "${mk}"
}

# pad_to <file> <length>: same result as "objcopy -I binary -O binary --pad-to <length>
# --gap-fill=0", which only ever grows a file.
pad_to() {
    local size
    size="$(stat -c %s "$1")"
    if (( size < $2 )); then truncate -s "$(( $2 ))" "$1"; fi
}

# make_ddr_fw <output file>: prints "nxp" or "zero" depending on the source used.
make_ddr_fw() {
    local out="$1" pkg mk cfgin kind imem_len dmem_len tmp
    pkg="$(repo_path "$(cfg '.boot.packages.firmware_imx')")"
    mk="${pkg}/firmware-imx.mk"
    cfgin="${pkg}/Config.in"
    kind=LPDDR4
    [[ "$(br BR2_PACKAGE_FIRMWARE_IMX_DDR4)" == "y" ]] && kind=DDR4
    imem_len="$(br BR2_PACKAGE_FIRMWARE_IMX_IMEM_LEN)"
    dmem_len="$(br BR2_PACKAGE_FIRMWARE_IMX_DMEM_LEN)"
    : "${imem_len:=$(kconfig_default "${cfgin}" BR2_PACKAGE_FIRMWARE_IMX_IMEM_LEN)}"
    : "${dmem_len:=$(kconfig_default "${cfgin}" BR2_PACKAGE_FIRMWARE_IMX_DMEM_LEN)}"

    local triples
    triples="$(ddr_fw_triples "${mk}" "${kind}")"
    [[ -n "${triples}" ]] || die "imxboot: no DDR firmware recipe found for ${kind} in ${mk}"

    tmp="$(mktemp -d)"
    : > "${out}"
    local imem dmem name source=zero
    [[ -n "${DDR_FW_DIR:-}" ]] && source=nxp
    while read -r imem dmem name; do
        if [[ "${source}" == nxp ]]; then
            [[ -f "${DDR_FW_DIR}/${imem}.bin" && -f "${DDR_FW_DIR}/${dmem}.bin" ]] \
                || die "imxboot: ${imem}.bin or ${dmem}.bin missing in the DDR firmware directory"
            cp "${DDR_FW_DIR}/${imem}.bin" "${tmp}/i.bin"
            cp "${DDR_FW_DIR}/${dmem}.bin" "${tmp}/d.bin"
        else
            : > "${tmp}/i.bin"
            : > "${tmp}/d.bin"
        fi
        pad_to "${tmp}/i.bin" "${imem_len}"
        pad_to "${tmp}/d.bin" "${dmem_len}"
        cat "${tmp}/i.bin" "${tmp}/d.bin" >> "${out}"
        log "imxboot: ddr fw ${name}: imem ${imem_len} + dmem ${dmem_len} (${source})"
    done <<< "${triples}"
    rm -rf "${tmp}"
    echo "${source}"
}

# bootloader_script: the post-image script of the defconfig that assembles imx-boot
# (the one calling mkimage_imx8).
bootloader_script() {
    local s
    for s in $(br BR2_ROOTFS_POST_IMAGE_SCRIPT); do
        if grep -q mkimage_imx8 "${BR_DIR}/${s}"; then
            echo "${BR_DIR}/${s}"
            return
        fi
    done
    die "imxboot: no post-image script with mkimage_imx8 in the defconfig"
}

stage_imxboot() {
    local stage="${WORK}/build/imxboot"
    local hostbin="${stage}/host/bin" images="${stage}/images" top="${stage}/top"
    rm -rf "${stage}"
    mkdir -p "${images}" "${top}"

    [[ -x "${UBOOT_SRC}/tools/mkimage" ]] || die "imxboot: build the uboot stage first"
    local plat
    plat="$(br BR2_TARGET_ARM_TRUSTED_FIRMWARE_PLATFORM)"
    [[ -f "${WORK}/src/atf/build/${plat}/release/bl31.bin" ]] || die "imxboot: build the atf stage first"
    mkimage_host_tools "${hostbin}"

    cp "${UBOOT_SRC}/spl/u-boot-spl.bin" "${images}/"
    cp "${UBOOT_SRC}/$(br BR2_TARGET_UBOOT_FORMAT_CUSTOM_NAME)" "${images}/"
    cp "${WORK}/src/atf/build/${plat}/release/bl31.bin" "${images}/"
    local fw_source
    fw_source="$(make_ddr_fw "${images}/ddr_fw.bin")"

    # Buildroot calls post-image scripts from its top directory with BINARIES_DIR as the
    # first argument followed by BR2_ROOTFS_POST_SCRIPT_ARGS. UBOOT_DIR is expanded the
    # way Buildroot's make would.
    local script args
    script="$(bootloader_script)"
    args="$(br BR2_ROOTFS_POST_SCRIPT_ARGS)"
    args="${args//\$\{UBOOT_DIR\}/${UBOOT_SRC}}"
    log "imxboot: running $(basename "${script}")"
    # shellcheck disable=SC2086 # args is a word list, as in Buildroot
    (cd "${top}" && BR2_CONFIG="${DEFCONFIG}" BINARIES_DIR="${images}" \
        HOST_DIR="${stage}/host" PATH="${hostbin}:${PATH}" \
        bash "${script}" "${images}" ${args})

    local bin="${images}/imx8-boot-sd.bin"
    [[ -f "${bin}" ]] || die "imxboot: ${script} did not produce imx8-boot-sd.bin"
    publish "${bin}" imx-boot
    publish "${images}/u-boot.itb"
    publish "${images}/u-boot-spl-ddr.bin"
    {
        echo "ddr_firmware=${fw_source}"
        if [[ "${fw_source}" == zero ]]; then
            echo "note=zero-filled DDR training blobs; this imx-boot is valid for the emulator only"
        fi
    } > "${stage}/imx-boot.info"
    publish "${stage}/imx-boot.info"

    if [[ "${fw_source}" == zero ]]; then
        cat >&2 <<'EOF'

  ********************************************************************************
  *  imx-boot was built WITHOUT the NXP DDR training firmware.                   *
  *  The training blobs are zero-filled placeholders of the correct size.        *
  *  This image is ONLY valid for the KICK emulator, whose DDR PHY model does   *
  *  not execute training firmware. It will NOT boot real hardware.              *
  *  To build a hardware-capable image, pass --ddr-fw <dir> or set              *
  *  boot.ddr_firmware_dir (see Emulator/boot/README.md).                       *
  ********************************************************************************

EOF
    fi
}
