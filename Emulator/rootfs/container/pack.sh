#!/usr/bin/env bash
#
# Inside a throwaway container (started by Emulator/rootfs/build.sh): reads the exported
# stage container (docker export, tar on stdin), replaces what docker manages inside a
# container (hostname, hosts, resolv.conf, /dev, /.dockerenv) and packs the tree into an
# ext4 image with mke2fs -d.
#
# Environment: IMAGE_SIZE, LABEL, KICK_HOSTNAME. Mounts: /out (output directory).

set -euo pipefail

log() { echo "[rootfs] $*" >&2; }

: "${IMAGE_SIZE:?}" "${LABEL:?}" "${KICK_HOSTNAME:?}"
ROOT=/stage

mkdir -p "${ROOT}"
tar -x -C "${ROOT}" --numeric-owner -p -f -

rm -f "${ROOT}/.dockerenv"
rm -rf "${ROOT}/dev" "${ROOT}/proc" "${ROOT}/sys" "${ROOT}/run" "${ROOT}/tmp"
mkdir -p "${ROOT}/dev" "${ROOT}/proc" "${ROOT}/sys" "${ROOT}/run"
install -d -m 1777 "${ROOT}/tmp"
# The kernel needs /dev/console before devtmpfs is mounted.
mknod -m 600 "${ROOT}/dev/console" c 5 1
mknod -m 666 "${ROOT}/dev/null" c 1 3

echo "${KICK_HOSTNAME}" > "${ROOT}/etc/hostname"
cat > "${ROOT}/etc/hosts" <<EOF
127.0.0.1	localhost
127.0.1.1	${KICK_HOSTNAME}
::1		localhost ip6-localhost ip6-loopback
EOF
# systemd-resolved owns name resolution (DHCP DNS from systemd-networkd).
rm -f "${ROOT}/etc/resolv.conf"
ln -s ../run/systemd/resolve/stub-resolv.conf "${ROOT}/etc/resolv.conf"

log "creating ext4 (${IMAGE_SIZE}) from $(du -sh "${ROOT}" | cut -f1)"
rm -f /out/.rootfs.ext4
mke2fs -q -t ext4 -L "${LABEL}" -d "${ROOT}" /out/.rootfs.ext4 "${IMAGE_SIZE}"
mv -f /out/.rootfs.ext4 /out/rootfs.ext4
