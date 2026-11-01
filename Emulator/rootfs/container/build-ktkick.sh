#!/usr/bin/env bash
#
# Inside a debian:bookworm arm64 container (started by Emulator/rootfs/build.sh app with
# app source 'source'): builds the KICK application from a git repository and packages it
# like the fork's CI job "KT_Kick (Linux arm64)" (.github/workflows/build.yml).
#
# Environment: REPO_URL, REPO_REF, PACKAGE_NAME, PACKAGES, JOBS (0: by CPUs and memory),
#              MEM_PER_JOB_MB, BUILD_IMAGE.
# Mounts: /work (named volume: sources, CPM cache, build tree), /out (package output).

set -euo pipefail

log() { echo "[ktkick] $*" >&2; }

: "${REPO_URL:?}" "${REPO_REF:?}" "${PACKAGE_NAME:?}" "${PACKAGES:?}" "${MEM_PER_JOB_MB:?}"
SRC=/work/src

log "installing build packages"
apt-get update -qq
# shellcheck disable=SC2086 # the package list is split on purpose
DEBIAN_FRONTEND=noninteractive apt-get install -y -qq --no-install-recommends ${PACKAGES} >/dev/null

# Fetch exactly the requested ref (branch, tag or commit) into a reused clone.
if [[ ! -d "${SRC}/.git" ]]; then
    git init -q "${SRC}"
    git -C "${SRC}" remote add origin "${REPO_URL}"
fi
git -C "${SRC}" remote set-url origin "${REPO_URL}"
log "fetching ${REPO_URL} ${REPO_REF}"
git -C "${SRC}" fetch -q --depth 1 origin "${REPO_REF}"
git -C "${SRC}" checkout -q --force FETCH_HEAD
sha="$(git -C "${SRC}" rev-parse HEAD)"
log "commit ${sha}"

# The workflow's job count: one compiler per CPU, and per MEM_PER_JOB_MB of memory (-O3 -flto).
if [[ "${JOBS}" == 0 ]]; then
    cpus="$(nproc)"
    by_mem=$(( $(awk '/MemTotal/ {print $2}' /proc/meminfo) / 1024 / MEM_PER_JOB_MB ))
    [[ "${by_mem}" -ge 1 ]] || by_mem=1
    JOBS=$(( cpus < by_mem ? cpus : by_mem ))
fi
log "building with ${JOBS} jobs"

cd "${SRC}"
cmake -S Kick -B Kick/build -G Ninja -DCMAKE_BUILD_TYPE=Release -DTIMESTAMP=OFF \
    -DCPM_SOURCE_CACHE=/work/cpm-cache
cmake --build Kick/build --target Kick --parallel "${JOBS}"
bin=Kick/build/apps/kick/Kick_artefacts/Release/Kick
file "${bin}"

# Package: the binary as KT_Kick plus the data it reads from /app at run time.
dist=/work/dist
pkg="${dist}/${PACKAGE_NAME}"
rm -rf "${dist}"
mkdir -p "${pkg}"
cp "${bin}" "${pkg}/KT_Kick"
cp -R Kick/assets Kick/templates Kick/demos "${pkg}/"
ldd "${pkg}/KT_Kick" | awk '{print $1}' | sort > "${pkg}/shared-libraries.txt"
python3 - "${pkg}/manifest.json" "${REPO_URL}" "${REPO_REF}" "${sha}" "${BUILD_IMAGE:-}" <<'PY'
import datetime, json, platform, re, subprocess, sys
out, url, ref, sha, image = sys.argv[1:6]
def run(*cmd):
    return subprocess.run(cmd, capture_output=True, text=True).stdout.strip()
with open(out.rsplit("/", 1)[0] + "/KT_Kick", "rb") as f:
    needed = {tuple(map(int, m.groups())) for m in re.finditer(rb"GLIBC_(\d+)\.(\d+)\b", f.read())}
manifest = {
    "name": "KT_Kick",
    "description": "Kick app built for Linux arm64 in a %s container by Emulator/rootfs/build.sh" % image,
    "repository": url,
    "git_ref": ref,
    "git_sha": sha,
    "build_date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
    "build_type": "Release",
    "compiler": run("c++", "--version").splitlines()[0],
    "cmake": run("cmake", "--version").splitlines()[0],
    "container_image": image or None,
    "os": run("sh", "-c", ". /etc/os-release && echo $PRETTY_NAME"),
    "glibc": platform.libc_ver()[1],
    "glibc_required": "%d.%d" % max(needed) if needed else None,
    "arch": platform.machine(),
    "install": {"KT_Kick": "/usr/bin/KT_Kick", "assets": "/app/assets",
                "templates": "/app/templates", "demos": "/app/demos"},
}
with open(out, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PY
tar -C "${dist}" -czf "/out/.${PACKAGE_NAME}.tar.gz" "${PACKAGE_NAME}"
mv -f "/out/.${PACKAGE_NAME}.tar.gz" "/out/${PACKAGE_NAME}.tar.gz"
cp "${pkg}/manifest.json" /out/manifest.json
log "wrote /out/${PACKAGE_NAME}.tar.gz"
