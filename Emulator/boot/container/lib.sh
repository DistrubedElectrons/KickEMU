# Helpers used inside the builder container.
#
# Layout of the build volume (/work):
#   src/<name>     pristine sources at the pinned revision with the board patches applied
#   build/<name>   out-of-tree objects (kernel) and staging directories
#   dl/            downloaded archives
#
# shellcheck shell=bash

WORK="${WORK:-/work}"
OUT="${OUT:-/out}"
mkdir -p "${WORK}/src" "${WORK}/build" "${WORK}/dl"

if [[ -z "${JOBS:-}" || "${JOBS}" == "0" ]]; then
    JOBS="$(nproc)"
fi

DEFCONFIG="$(repo_path "$(cfg '.boot.defconfig')")"
BOARD_DIR="$(repo_path "$(cfg '.boot.board_dir')")"
# Paths inside the defconfig are relative to the Buildroot top directory.
BR_DIR="$(cd "$(dirname "${DEFCONFIG}")/.." && pwd)"
[[ -f "${DEFCONFIG}" ]] || die "defconfig not found: ${DEFCONFIG}"

br() { br_get "${DEFCONFIG}" "$1"; }
br_path() { printf '%s/%s\n' "${BR_DIR}" "$(br "$1")"; }

# fetch_git <name> <url> <revision>
# Shallow fetch of exactly one commit or tag into src/<name>. The .git directory is
# removed afterwards: Buildroot builds from a tarball, so the kernel version string must
# not pick up a git suffix, and it saves space. A stamp file records what is there.
fetch_git() {
    local name="$1" url="$2" rev="$3" dir="${WORK}/src/$1"
    local stamp="${dir}/.kickemu-source"
    if [[ -f "${stamp}" && "$(cat "${stamp}")" == "${url} ${rev}" ]]; then
        log "${name}: source ${rev} already present"
        return 0
    fi
    log "${name}: fetching ${url} at ${rev}"
    rm -rf "${dir}"
    mkdir -p "${dir}"
    git -C "${dir}" init -q
    git -C "${dir}" fetch -q --depth 1 "${url}" "${rev}"
    git -C "${dir}" -c advice.detachedHead=false checkout -q FETCH_HEAD
    rm -rf "${dir}/.git"
    printf '%s %s\n' "${url}" "${rev}" > "${stamp}"
    rm -f "${dir}/.kickemu-patched"
}

# fetch_tarball <name> <url> <file name> [sha256]
# Downloads an archive into dl/ (verified when a hash is given) and unpacks it into
# src/<name> with the top-level directory stripped.
fetch_tarball() {
    local name="$1" url="$2" file="$3" sha="${4:-}" dir="${WORK}/src/$1"
    local stamp="${dir}/.kickemu-source"
    if [[ -f "${stamp}" && "$(cat "${stamp}")" == "${url}" ]]; then
        log "${name}: source already present"
        return 0
    fi
    if [[ ! -f "${WORK}/dl/${file}" ]]; then
        log "${name}: downloading ${url}"
        curl -fsSL -o "${WORK}/dl/${file}.part" "${url}"
        mv "${WORK}/dl/${file}.part" "${WORK}/dl/${file}"
    fi
    if [[ -n "${sha}" ]]; then
        echo "${sha}  ${WORK}/dl/${file}" | sha256sum -c --quiet - \
            || die "${name}: sha256 mismatch for ${file}"
    fi
    rm -rf "${dir}"
    mkdir -p "${dir}"
    tar -xf "${WORK}/dl/${file}" -C "${dir}" --strip-components=1
    printf '%s\n' "${url}" > "${stamp}"
    rm -f "${dir}/.kickemu-patched"
}

# hash_get <file.hash> <file name>: sha256 for a file from a Buildroot .hash file.
hash_get() {
    awk -v f="$2" '$1 == "sha256" && $3 == f { print $2 }' "$1"
}

# apply_patches <name> <patch dir or file>...
# Applies patches the same way Buildroot's support/scripts/apply-patches.sh does
# (sorted, patch -p1 with the same options), once per fetched source.
apply_patches() {
    local name="$1" dir="${WORK}/src/$1" p; shift
    [[ -f "${dir}/.kickemu-patched" ]] && return 0
    local patches=()
    for p in "$@"; do
        if [[ -d "${p}" ]]; then
            while IFS= read -r f; do patches+=("${f}"); done \
                < <(find "${p}" -maxdepth 1 -type f -name '*.patch' | LC_ALL=C sort)
        elif [[ -f "${p}" ]]; then
            patches+=("${p}")
        fi
    done
    for p in "${patches[@]+"${patches[@]}"}"; do
        log "${name}: applying $(basename "${p}")"
        patch -g0 -p1 -E --no-backup-if-mismatch -d "${dir}" -t -N -s < "${p}"
    done
    touch "${dir}/.kickemu-patched"
}

# publish <file> [dest name]: copies one artifact to the output directory atomically, so
# other engineers never pick up a half-written file.
publish() {
    local src="$1" name="${2:-$(basename "$1")}"
    cp "${src}" "${OUT}/.${name}.tmp"
    mv -f "${OUT}/.${name}.tmp" "${OUT}/${name}"
    log "published ${name} ($(du -h "${OUT}/${name}" | cut -f1))"
}
