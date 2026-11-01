# The rootfs step of Emulator/rootfs/build.sh: install everything in a named container
# (stage_rootfs), then export that container's file system and pack it as ext4 (pack_rootfs).
#
# Expected variables: REPO_ROOT, BOOT_OUT, OUT_DIR, APP_PACKAGE, ROOTFS_IMAGE, PLATFORM;
# common.sh and docker.sh sourced.
#
# shellcheck shell=bash

stage_container() { cfg '.rootfs.stage_container'; }

# app_environment: KEY=VALUE lines for /etc/default/ktkick. DISPLAY comes from .rootfs.x.
app_environment() {
    printf 'DISPLAY=%s\n' "$(cfg '.rootfs.x.display')"
    jq -r '(.rootfs.app.environment // {}) | to_entries[] | "\(.key)=\(.value)"' "${KICKEMU_CONFIG}"
}

stage_rootfs() {
    local release keys_file keys="" board
    [[ -f "${BOOT_OUT}/kernel.release" ]] || die "no kernel build in ${BOOT_OUT}; run Emulator/boot/build.sh kernel gpu"
    release="$(cat "${BOOT_OUT}/kernel.release")"
    [[ -f "${BOOT_OUT}/modules-${release}.tar.gz" ]] || die "modules tarball for ${release} missing in ${BOOT_OUT}"
    [[ -f "${APP_PACKAGE}" ]] || die "no KT_Kick package ${APP_PACKAGE}; run the app step first"
    board="$(repo_path "$(cfg '.boot.board_dir')")/rootfs_overlay"
    [[ -d "${board}" ]] || die "board overlay not found: ${board}"

    keys_file="$(cfg '.rootfs.ssh_authorized_keys' '')"
    if [[ -n "${keys_file}" ]]; then
        keys_file="${keys_file/#\~/${HOME}}"
        [[ -f "${keys_file}" ]] || die "ssh_authorized_keys file not found: ${keys_file}"
        keys="$(cat "${keys_file}")"
    fi

    docker rm -f "$(stage_container)" >/dev/null 2>&1 || true
    log "staging $(cfg '.rootfs.base_image') in container $(stage_container), kernel ${release}"
    docker run --name "$(stage_container)" \
        --platform "${PLATFORM}" \
        -v "${REPO_ROOT}/Emulator/rootfs:/rootfs-src:ro" \
        -v "${board}:/board:ro" \
        -v "${BOOT_OUT}:/boot-out:ro" \
        -v "$(dirname "${APP_PACKAGE}"):/app-pkg:ro" \
        -v "${OUT_DIR}:/out" \
        -e "PACKAGES=$(cfg_list '.rootfs.packages' | tr '\n' ' ')" \
        -e "APT_MIRROR=$(cfg '.rootfs.apt_mirror' '')" \
        -e "KERNEL_RELEASE=${release}" \
        -e "KICK_HOSTNAME=$(cfg '.rootfs.hostname')" \
        -e "ROOT_PASSWORD=$(cfg '.rootfs.root_password' '')" \
        -e "AUTHORIZED_KEYS=${keys}" \
        -e "CONSOLE_TTY=$(cfg '.rootfs.console_tty')" \
        -e "CONSOLE_AUTOLOGIN=$(cfg '.rootfs.console_autologin' false)" \
        -e "BOARD_FILES=$(cfg_list '.rootfs.board_overlay_files')" \
        -e "X_DISPLAY=$(cfg '.rootfs.x.display')" \
        -e "X_VT=$(cfg '.rootfs.x.vt')" \
        -e "X_FBDEV=$(cfg '.rootfs.x.fbdev')" \
        -e "X_FB_KERNEL=$(cfg '.rootfs.x.fb_kernel_name')" \
        -e "X_TOUCH_PRODUCT=$(cfg '.rootfs.x.touch_product')" \
        -e "X_ARGS=$(cfg_list '.rootfs.x.server_args' | tr '\n' ' ')" \
        -e "APP_ENV=$(app_environment)" \
        -e "APP_PACKAGE=$(basename "${APP_PACKAGE}")" \
        "$(cfg '.rootfs.base_image')" bash /rootfs-src/container/mkrootfs.sh
}

pack_rootfs() {
    log "exporting $(stage_container) and packing ${ROOTFS_IMAGE} ($(cfg '.rootfs.image_size'))"
    docker export "$(stage_container)" | docker run --rm -i \
        --platform "${PLATFORM}" \
        -v "${REPO_ROOT}/Emulator/rootfs:/rootfs-src:ro" \
        -v "${OUT_DIR}:/out" \
        -e "IMAGE_SIZE=$(cfg '.rootfs.image_size')" \
        -e "LABEL=$(cfg '.rootfs.label')" \
        -e "KICK_HOSTNAME=$(cfg '.rootfs.hostname')" \
        "$(cfg '.rootfs.base_image')" bash /rootfs-src/container/pack.sh
    docker rm "$(stage_container)" >/dev/null
    ls -l "${ROOTFS_IMAGE}"
}
