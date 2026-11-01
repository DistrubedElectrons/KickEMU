# ATF stage: varigit imx-atf at the pinned commit, BL31 for the platform named in the
# defconfig, built with the same make variables Buildroot's arm-trusted-firmware package
# passes for this configuration (release build, aarch64, Armv8, no stack protector).
#
# shellcheck shell=bash

stage_atf() {
    local url rev plat src="${WORK}/src/atf"
    url="$(rewrite_url "$(br BR2_TARGET_ARM_TRUSTED_FIRMWARE_CUSTOM_REPO_URL)")"
    rev="$(br BR2_TARGET_ARM_TRUSTED_FIRMWARE_CUSTOM_REPO_VERSION)"
    plat="$(br BR2_TARGET_ARM_TRUSTED_FIRMWARE_PLATFORM)"
    fetch_git atf "${url}" "${rev}"

    local targets=(all)
    [[ "$(br BR2_TARGET_ARM_TRUSTED_FIRMWARE_BL31)" == "y" ]] && targets+=(bl31)
    log "atf: building ${targets[*]} for ${plat}"
    make -C "${src}" -j"${JOBS}" CROSS_COMPILE= PLAT="${plat}" \
        ARM_ARCH_MAJOR=8 ARCH=aarch64 "${targets[@]}"
    publish "${src}/build/${plat}/release/bl31.bin"
    publish "${src}/build/${plat}/release/bl31/bl31.elf"
}
