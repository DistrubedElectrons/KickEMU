#!/usr/bin/env bash
#
# Entry point inside the builder container. Started by Emulator/boot/build.sh; not meant
# to be run on the host. Arguments are stage names (kernel gpu atf uboot imxboot, or all).

set -euo pipefail

: "${REPO_ROOT:=/repo}"
LOG_TAG="boot"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# shellcheck source=../lib/common.sh
source "${HERE}/../lib/common.sh"
# shellcheck source=lib.sh
source "${HERE}/lib.sh"
for stage_file in "${HERE}"/stage-*.sh; do
    # shellcheck disable=SC1090
    source "${stage_file}"
done

log "toolchain: $(gcc --version | head -n 1)"

stages=("$@")
[[ ${#stages[@]} -eq 0 || " ${stages[*]} " == *" all "* ]] && stages=(kernel gpu atf uboot imxboot)

for s in "${stages[@]}"; do
    log "=== stage ${s} ==="
    "stage_${s}"
done
log "done; outputs in the configured output directory"
