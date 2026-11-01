# GPU stage: the Vivante galcore kernel module from Buildroot's kernel-module-imx-gpu-viv
# package, built against the kernel from the kernel stage. Version and download location
# are read from the package's .mk file, the archive is checked against its .hash file.
#
# shellcheck shell=bash

stage_gpu() {
    local pkg mk name version owner_repo url file sha
    pkg="$(repo_path "$(cfg '.boot.packages.gpu_module')")"
    name="$(basename "${pkg}")"
    mk="${pkg}/${name}.mk"
    version="$(mk_get "${mk}" KERNEL_MODULE_IMX_GPU_VIV_VERSION)"
    # The site is "$(call github,<owner>,<repo>,$(VERSION))" on a continuation line.
    owner_repo="$(grep -o 'call github,[^,]*,[^,]*' "${mk}" | sed 's/^call github,//; s/,/\//')"
    [[ -n "${version}" && -n "${owner_repo}" ]] || die "gpu: cannot parse ${mk}"
    url="https://github.com/${owner_repo}/archive/${version}.tar.gz"
    file="${name}-${version}.tar.gz"
    sha="$(hash_get "${pkg}/${name}.hash" "${file}")"
    fetch_tarball "${name}" "${url}" "${file}" "${sha}"

    [[ -f "${LINUX_OBJ}/Module.symvers" ]] || die "gpu: build the kernel stage first"

    # Same invocation as Buildroot's kernel-module infrastructure with the package's
    # MODULE_MAKE_OPTS (AQROOT, KERNEL_DIR) and MODULE_SUBDIRS.
    local src="${WORK}/src/${name}" sub
    sub="$(mk_get "${mk}" KERNEL_MODULE_IMX_GPU_VIV_MODULE_SUBDIRS)"
    log "gpu: building ${sub} ${version}"
    make -C "${LINUX_OBJ}" ARCH=arm64 -j"${JOBS}" \
        AQROOT="${src}/${sub}" KERNEL_DIR="${LINUX_SRC}" \
        PWD="${src}/${sub}" M="${src}/${sub}" modules

    local ko
    ko="$(find "${src}/${sub}" -name galcore.ko -print -quit)"
    [[ -n "${ko}" ]] || die "gpu: galcore.ko was not produced"
    strip --strip-debug -o "${WORK}/build/galcore.ko" "${ko}"
    publish "${WORK}/build/galcore.ko"
}
