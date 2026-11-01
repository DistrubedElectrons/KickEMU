# Kernel stage: varigit linux-imx at the pinned commit, the board patches and the board
# kernel configuration, all as referenced by the defconfig. Builds the image format
# selected there, the in-tree DTBs listed there, and the modules.
#
# shellcheck shell=bash

LINUX_SRC="${WORK}/src/linux"
LINUX_OBJ="${WORK}/build/linux"

linux_make() {
    # Native aarch64 build: ARCH is set, CROSS_COMPILE stays empty so the container's
    # GCC 10.2 is used for both target and host tools.
    make -C "${LINUX_SRC}" O="${LINUX_OBJ}" ARCH=arm64 -j"${JOBS}" "$@"
}

# Kernel image target, mirroring Buildroot's BR2_LINUX_KERNEL_<format> choice for arm64.
linux_image_name() {
    if [[ "$(br BR2_LINUX_KERNEL_IMAGEGZ)" == "y" ]]; then
        echo Image.gz
    else
        echo Image
    fi
}

stage_kernel() {
    local url rev
    url="$(rewrite_url "$(br BR2_LINUX_KERNEL_CUSTOM_REPO_URL)")"
    rev="$(br BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION)"
    fetch_git linux "${url}" "${rev}"
    apply_patches linux "$(br_path BR2_LINUX_KERNEL_PATCH)"

    # Buildroot installs the custom config as .config and runs olddefconfig. The board
    # configuration is used unmodified.
    mkdir -p "${LINUX_OBJ}"
    local kconfig
    kconfig="$(br_path BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE)"
    if ! cmp -s "${kconfig}" "${LINUX_OBJ}/.kickemu-defconfig"; then
        cp "${kconfig}" "${LINUX_OBJ}/.config"
        cp "${kconfig}" "${LINUX_OBJ}/.kickemu-defconfig"
        linux_make olddefconfig
    fi

    local image dts dtbs=()
    image="$(linux_image_name)"
    for dts in $(br BR2_LINUX_KERNEL_INTREE_DTS_NAME); do
        dtbs+=("${dts}.dtb")
    done
    log "kernel: building ${image} and ${#dtbs[@]} DTBs with ${JOBS} jobs"
    linux_make "${image}" "${dtbs[@]}"

    # Image and DTBs first: the SoC model work only needs these.
    local boot="${LINUX_OBJ}/arch/arm64/boot"
    publish "${boot}/Image"
    [[ "${image}" == "Image" ]] || publish "${boot}/${image}"
    for dts in "${dtbs[@]}"; do
        publish "${boot}/dts/${dts}"
    done

    log "kernel: building modules"
    linux_make modules
    local release stage="${WORK}/build/linux-modules"
    release="$(cat "${LINUX_OBJ}/include/config/kernel.release")"
    rm -rf "${stage}"
    linux_make INSTALL_MOD_PATH="${stage}" INSTALL_MOD_STRIP=1 modules_install
    # The build/source links point into the build volume and are useless on a target.
    rm -f "${stage}/lib/modules/${release}/build" "${stage}/lib/modules/${release}/source"
    tar -C "${stage}" --owner=0 --group=0 -czf "${WORK}/build/modules.tar.gz" lib
    publish "${WORK}/build/modules.tar.gz" "modules-${release}.tar.gz"
    rm -rf "${stage}"

    publish "${LINUX_OBJ}/.config" kernel.config
    publish "${LINUX_OBJ}/System.map"
    printf '%s\n' "${release}" > "${WORK}/build/kernel.release"
    publish "${WORK}/build/kernel.release"
}
