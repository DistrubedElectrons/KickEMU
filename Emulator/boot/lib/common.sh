# Shared helpers for the boot chain scripts. Sourced on the host and inside the
# builder container; it must not depend on anything but bash, coreutils, sed and jq.
#
# Expected variables before sourcing:
#   REPO_ROOT    root of the checkout the scripts run from (sources, board files)
#   KICKEMU_CONFIG (optional) path to the JSON configuration
#
# shellcheck shell=bash

: "${REPO_ROOT:?REPO_ROOT must be set before sourcing common.sh}"
KICKEMU_CONFIG="${KICKEMU_CONFIG:-${REPO_ROOT}/Emulator/config/kickemu.json}"

log()  { printf '[%s] %s\n' "${LOG_TAG:-kickemu}" "$*" >&2; }
die()  { printf '[%s] ERROR: %s\n' "${LOG_TAG:-kickemu}" "$*" >&2; exit 1; }

# cfg <jq path> [default]
# Reads one value from the JSON configuration. Missing keys and null return the default
# (or fail when no default is given), so every tunable has exactly one source.
cfg() {
    local path="$1" value
    command -v jq >/dev/null || die "jq is required to read ${KICKEMU_CONFIG}"
    value="$(jq -r "${path} // empty" "${KICKEMU_CONFIG}")"
    if [[ -z "${value}" ]]; then
        [[ $# -ge 2 ]] || die "missing configuration key ${path} in ${KICKEMU_CONFIG}"
        value="$2"
    fi
    printf '%s\n' "${value}"
}

# cfg_list <jq path>: prints one array element per line (empty when the key is absent).
cfg_list() {
    jq -r "(${1} // [])[]" "${KICKEMU_CONFIG}"
}

# repo_path <path>: absolute paths are kept, relative ones resolve against REPO_ROOT.
repo_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "${REPO_ROOT}/$1" ;;
    esac
}

# br_get <file> <VAR>: value of VAR from a Buildroot defconfig (y/strings), unquoted.
br_get() {
    sed -n "s/^$2=\"\{0,1\}\([^\"]*\)\"\{0,1\}\$/\1/p" "$1" | tail -n 1
}

# mk_get <file.mk> <VAR>: value of a simple "VAR = value" assignment in a Buildroot .mk.
mk_get() {
    sed -n "s/^$2[[:space:]]*=[[:space:]]*\(.*\)\$/\1/p" "$1" | tail -n 1
}

# kconfig_default <Config.in> <SYMBOL>: first plain "default" of a Buildroot symbol.
kconfig_default() {
    awk -v sym="config $2" '
        $0 == sym          { inside = 1; next }
        inside && /^config / { exit }
        inside && $1 == "default" { print $2; exit }
    ' "$1"
}

# rewrite_url <url>: applies the url_rewrites table from the configuration. Buildroot
# 2021.02 still points at hosts that moved (source.codeaurora.org) or protocols GitHub
# dropped (git://), so the old URLs are mapped to their current location.
rewrite_url() {
    local url="$1" from to
    while IFS=$'\t' read -r from to; do
        [[ -n "${from}" ]] || continue
        if [[ "${url}" == "${from}"* ]]; then
            url="${to}${url#"${from}"}"
            break
        fi
    done < <(jq -r '(.boot.url_rewrites // {}) | to_entries[] | "\(.key)\t\(.value)"' "${KICKEMU_CONFIG}")
    printf '%s\n' "${url}"
}

# main_checkout_root: the primary working tree of this repository. Outputs default to it
# so that builds started from any git worktree land where every engineer looks for them.
main_checkout_root() {
    local common
    common="$(git -C "${REPO_ROOT}" rev-parse --path-format=absolute --git-common-dir 2>/dev/null)" || {
        printf '%s\n' "${REPO_ROOT}"; return
    }
    (cd "${common}/.." && pwd)
}

# out_path <path>: like repo_path, but relative paths resolve against the main checkout.
out_path() {
    case "$1" in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s\n' "$(main_checkout_root)/$1" ;;
    esac
}
