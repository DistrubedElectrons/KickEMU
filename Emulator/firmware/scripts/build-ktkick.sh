#!/usr/bin/env bash
#
# Cross-builds KT_Kick against the Buildroot SDK made by br-build.sh and packages it in the
# layout Emulator/rootfs uses: KT_Kick, assets/, templates/, demos/, manifest.json.
#
# Usage: build-ktkick.sh <sdk.tar.gz> <work dir> <output dir>
# Env:   KTKICK_REPO, KTKICK_REF  source of the app (see the workflow for why the fork)
#        CPM_SOURCE_CACHE         JUCE download cache (default: <work dir>/cpm)
#        JOBS                     compile jobs (default: CPUs, at most one per 2.5 GB of RAM)
#
# The build host needs cmake, ninja, git, python3 and a native C++ compiler with the X11 and
# freetype headers: JUCE builds its helper tool juceaide for the host during configuration.

set -euo pipefail

log() { echo "[ktkick] $*" >&2; }

[[ $# -eq 3 ]] || { sed -n '3,14p' "$0" >&2; exit 2; }
SDK_TAR="$(realpath "$1")"
WORK="$(realpath -m "$2")"
OUTDIR="$(realpath -m "$3")"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
APP_DIR="${HERE}/../app"
: "${KTKICK_REPO:?}" "${KTKICK_REF:?}"
CPM_SOURCE_CACHE="${CPM_SOURCE_CACHE:-${WORK}/cpm}"

mkdir -p "${WORK}" "${OUTDIR}"

# --- SDK ---------------------------------------------------------------------------------------
sdk="${WORK}/sdk"
rm -rf "${sdk}"
mkdir -p "${sdk}"
log "unpacking the SDK"
tar -xzf "${SDK_TAR}" -C "${sdk}" --strip-components=1
"${sdk}/relocate-sdk.sh" >/dev/null
export BR_SDK="${sdk}"

# --- sources -------------------------------------------------------------------------------------
src="${WORK}/src"
if [[ ! -d "${src}/.git" ]]; then
    git init -q "${src}"
    git -C "${src}" remote add origin "https://github.com/${KTKICK_REPO}.git"
fi
log "fetching ${KTKICK_REPO} ${KTKICK_REF}"
git -C "${src}" fetch -q --depth 1 origin "${KTKICK_REF}"
git -C "${src}" checkout -q --force FETCH_HEAD
sha="$(git -C "${src}" rev-parse HEAD)"

if [[ -z "${JOBS:-}" ]]; then
    cpus="$(nproc)"
    by_mem=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 2500000 ))
    [[ "${by_mem}" -ge 1 ]] || by_mem=1
    JOBS=$(( cpus < by_mem ? cpus : by_mem ))
fi

# --- build -------------------------------------------------------------------------------------
build="${WORK}/build"
log "configuring (commit ${sha})"
cmake -S "${src}/Kick" -B "${build}" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="${APP_DIR}/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DTIMESTAMP=OFF \
    -DKICK_LINK_TESTS=OFF \
    -DCPM_SOURCE_CACHE="${CPM_SOURCE_CACHE}"
log "building with ${JOBS} jobs"
cmake --build "${build}" --target Kick --parallel "${JOBS}"

bin="${build}/apps/kick/Kick_artefacts/Release/Kick"
file "${bin}"

# --- package -----------------------------------------------------------------------------------
pkg="${OUTDIR}/KT_Kick"
rm -rf "${pkg}"
mkdir -p "${pkg}"
strip="$(ls "${sdk}"/bin/*-strip | head -n 1)"
"${strip}" -o "${pkg}/KT_Kick" "${bin}"
cp -R "${src}/Kick/assets" "${src}/Kick/templates" "${src}/Kick/demos" "${pkg}/"

# Every shared library KT_Kick needs must be in the target sysroot (and so in the image).
readelf="$(ls "${sdk}"/bin/*-readelf | head -n 1)"
sysroot="$(ls -d "${sdk}"/*/sysroot | head -n 1)"
"${readelf}" -d "${pkg}/KT_Kick" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | sort > "${pkg}/shared-libraries.txt"
missing=0
while IFS= read -r lib; do
    if [[ -z "$(find "${sysroot}/lib" "${sysroot}/usr/lib" -maxdepth 1 -name "${lib}" -print -quit)" ]]; then
        echo "missing in the sysroot: ${lib}" >&2
        missing=1
    fi
done < "${pkg}/shared-libraries.txt"
[[ "${missing}" -eq 0 ]] || exit 1

compiler="$("$(ls "${sdk}"/bin/*-g++ | head -n 1)" --version | head -n 1)"
python3 - "${pkg}/manifest.json" "${KTKICK_REPO}" "${KTKICK_REF}" "${sha}" "${compiler}" <<'PY'
import datetime, json, sys
out, repo, ref, sha, compiler = sys.argv[1:6]
json.dump({
    "name": "KT_Kick",
    "description": "Kick app cross-built against the Buildroot SDK of the emulator image "
                   "(Emulator/firmware)",
    "repository": "https://github.com/" + repo,
    "git_ref": ref,
    "git_sha": sha,
    "build_date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "build_type": "Release",
    "compiler": compiler,
    "install": {"KT_Kick": "/usr/bin/KT_Kick", "assets": "/app/assets",
                "templates": "/app/templates", "demos": "/app/demos"},
}, open(out, "w"), indent=2)
open(out, "a").write("\n")
PY
tar -C "${OUTDIR}" -czf "${OUTDIR}/KT_Kick.tar.gz" KT_Kick
rm -rf "${pkg}"
log "wrote ${OUTDIR}/KT_Kick.tar.gz"
