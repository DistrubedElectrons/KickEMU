# Host-side helpers to build and run the boot chain builder container.
# Sourced by build.sh and mkimage-emmc.sh after lib/common.sh.
#
# shellcheck shell=bash

BOOT_DIR="${REPO_ROOT}/Emulator/boot"

builder_image()    { cfg '.boot.builder.image'; }
builder_platform() { cfg '.boot.builder.platform'; }
builder_volume()   { cfg '.boot.builder.volume'; }

ensure_docker() {
    command -v docker >/dev/null || die "docker CLI not found (install OrbStack or Docker)"
    if ! docker info >/dev/null 2>&1; then
        if command -v orb >/dev/null; then
            log "container runtime is not running, starting OrbStack"
            orb start >/dev/null
        fi
        docker info >/dev/null 2>&1 || die "docker daemon is not reachable"
    fi
}

# build_builder_image: (re)builds the builder image. Docker's layer cache makes this cheap
# when nothing changed. The genimage version comes from Buildroot's genimage.mk.
build_builder_image() {
    local mk version site source
    mk="$(repo_path "$(cfg '.boot.packages.genimage')")/genimage.mk"
    version="$(mk_get "${mk}" GENIMAGE_VERSION)"
    source="$(mk_get "${mk}" GENIMAGE_SOURCE)"
    site="$(mk_get "${mk}" GENIMAGE_SITE)"
    # Expand the $(GENIMAGE_VERSION) references used inside the .mk file.
    source="${source//\$(GENIMAGE_VERSION)/${version}}"
    site="${site//\$(GENIMAGE_VERSION)/${version}}"
    log "building builder image $(builder_image) ($(cfg '.boot.builder.base_image'), genimage ${version})"
    docker build --quiet \
        --platform "$(builder_platform)" \
        --build-arg "BASE_IMAGE=$(cfg '.boot.builder.base_image')" \
        --build-arg "APT_MIRROR=$(cfg '.boot.builder.apt_mirror')" \
        --build-arg "APT_SECURITY_MIRROR=$(cfg '.boot.builder.apt_security_mirror')" \
        --build-arg "GENIMAGE_VERSION=${version}" \
        --build-arg "GENIMAGE_URL=${site}/${source}" \
        -t "$(builder_image)" "${BOOT_DIR}" >/dev/null
}

# run_builder <out dir> [extra docker args...] -- <command...>
# Runs a command in the builder. The checkout is mounted read-only at /repo, the
# configuration at /cfg/kickemu.json, the output directory at /out and the named build
# volume (case-sensitive, unlike the default macOS file system) at /work.
run_builder() {
    local out="$1"; shift
    local extra=()
    while [[ $# -gt 0 && "$1" != "--" ]]; do extra+=("$1"); shift; done
    [[ "${1:-}" == "--" ]] && shift
    mkdir -p "${out}"
    local tty=()
    [[ -t 0 && -t 1 ]] && tty=(-t)
    docker run --rm -i ${tty[@]+"${tty[@]}"} \
        --platform "$(builder_platform)" \
        -v "${REPO_ROOT}:/repo:ro" \
        -v "${KICKEMU_CONFIG}:/cfg/kickemu.json:ro" \
        -v "${out}:/out" \
        -v "$(builder_volume):/work" \
        -e REPO_ROOT=/repo \
        -e KICKEMU_CONFIG=/cfg/kickemu.json \
        -e "JOBS=${JOBS}" \
        "${extra[@]+"${extra[@]}"}" \
        "$(builder_image)" "$@"
}
