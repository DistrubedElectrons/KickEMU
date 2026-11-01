# The KT_Kick package step of Emulator/rootfs/build.sh. Every source ends with the same
# file, ${APP_PACKAGE} (KT_Kick-linux-arm64.tar.gz in the layout of the fork's CI:
# KT_Kick-linux-arm64/{KT_Kick,assets,templates,demos,manifest.json}).
#
# Expected variables: OUT_DIR, APP_PACKAGE, PLATFORM; common.sh and docker.sh sourced.
#
# shellcheck shell=bash

# check_package <tar.gz>: the archive must hold the binary and the /app data directories.
check_package() {
    local listing top
    top="$(cfg '.rootfs.app.artifact')"
    listing="$(tar -tzf "$1")" || die "$1 is not a gzip tar archive"
    for entry in KT_Kick assets/ templates/ demos/; do
        grep -qx "${top}/${entry}" <<<"${listing}" || die "$1 has no ${top}/${entry}"
    done
}

# app_from_artifact: downloads the CI artifact. With .rootfs.app.run_id empty, the newest
# artifact of that name built from .rootfs.app.ref that has not expired yet is used.
app_from_artifact() {
    local repo workflow ref name run dl
    repo="$(cfg '.rootfs.app.repo')"
    workflow="$(cfg '.rootfs.app.workflow')"
    ref="$(cfg '.rootfs.app.ref')"
    name="$(cfg '.rootfs.app.artifact')"
    run="$(cfg '.rootfs.app.run_id' '')"
    command -v gh >/dev/null || die "the GitHub CLI (gh) is needed for app source 'artifact'"

    if [[ -z "${run}" ]]; then
        # Artifacts expire (30 days in the fork's workflow), so ask for live ones only.
        run="$(gh api "repos/${repo}/actions/artifacts?name=${name}&per_page=50" --jq \
            "[.artifacts[] | select(.expired | not) | select(.workflow_run.head_branch == \"${ref}\")][0].workflow_run.id // empty")"
        [[ -n "${run}" ]] || die "no unexpired ${name} artifact for ${repo}@${ref}; use app source 'source'"
    fi
    log "downloading ${name} from ${repo} workflow ${workflow}, run ${run}"
    dl="${OUT_DIR}/app/.download"
    rm -rf "${dl}"
    mkdir -p "${dl}"
    gh run download "${run}" -R "${repo}" -n "${name}" -D "${dl}"
    [[ -f "${dl}/${name}.tar.gz" ]] || die "artifact ${name} of run ${run} has no ${name}.tar.gz"
    check_package "${dl}/${name}.tar.gz"
    mv -f "${dl}/${name}.tar.gz" "${APP_PACKAGE}"
    [[ -f "${dl}/manifest.json" ]] && mv -f "${dl}/manifest.json" "${OUT_DIR}/app/manifest.json"
    rm -rf "${dl}"
    log "KT_Kick package: ${APP_PACKAGE}"
}

# app_from_file <tar.gz>: takes a package built elsewhere.
app_from_file() {
    local file="$1"
    [[ -n "${file}" ]] || die "app source 'file' needs --package or .rootfs.app.package_file"
    file="$(out_path "${file}")"
    [[ -f "${file}" ]] || die "package not found: ${file}"
    check_package "${file}"
    mkdir -p "$(dirname "${APP_PACKAGE}")"
    [[ "$(abspath "${file}")" == "${APP_PACKAGE}" ]] || cp -f "${file}" "${APP_PACKAGE}"
    log "KT_Kick package: ${APP_PACKAGE}"
}

# app_from_source: builds KT_Kick natively in a Debian 12 arm64 container from
# .rootfs.app.repo at .rootfs.app.ref, as the fork's .github/workflows/build.yml does.
# Sources, the CPM cache (JUCE) and the build tree stay in a named volume between runs.
app_from_source() {
    local volume
    volume="$(cfg '.rootfs.app.build_volume')"
    docker volume inspect "${volume}" >/dev/null 2>&1 || docker volume create "${volume}" >/dev/null
    mkdir -p "${OUT_DIR}/app"
    log "building KT_Kick from $(cfg '.rootfs.app.repo')@$(cfg '.rootfs.app.ref') in $(cfg '.rootfs.app.build_image')"
    docker run --rm \
        --platform "${PLATFORM}" \
        -v "${REPO_ROOT}/Emulator/rootfs/container:/scripts:ro" \
        -v "${volume}:/work" \
        -v "${OUT_DIR}/app:/out" \
        -e "REPO_URL=https://github.com/$(cfg '.rootfs.app.repo').git" \
        -e "REPO_REF=$(cfg '.rootfs.app.ref')" \
        -e "PACKAGE_NAME=$(cfg '.rootfs.app.artifact')" \
        -e "PACKAGES=$(cfg_list '.rootfs.app.build_packages' | tr '\n' ' ')" \
        -e "JOBS=$(cfg '.rootfs.app.build_jobs' 0)" \
        -e "MEM_PER_JOB_MB=$(cfg '.rootfs.app.build_mem_per_job_mb')" \
        -e "BUILD_IMAGE=$(cfg '.rootfs.app.build_image')" \
        "$(cfg '.rootfs.app.build_image')" bash /scripts/build-ktkick.sh
    check_package "${APP_PACKAGE}"
    log "KT_Kick package: ${APP_PACKAGE}"
}
