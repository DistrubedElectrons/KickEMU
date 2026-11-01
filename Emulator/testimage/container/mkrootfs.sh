#!/bin/sh
#
# Runs inside an Alpine aarch64 container (started by Emulator/testimage/build.sh).
# Builds the test root file system from Alpine packages, the kernel modules and galcore
# from the boot build, and the files in Emulator/testimage/rootfs, then packs it as an
# initramfs (cpio.gz) and as an ext4 image.
#
# Environment: PACKAGES (space separated), EXT4_SIZE, KERNEL_RELEASE.
# Mounts: /repo (checkout, read-only), /boot-out (boot outputs, read-only), /out.

set -eu

log() { echo "[testimage] $*" >&2; }

: "${PACKAGES:?}" "${EXT4_SIZE:?}" "${KERNEL_RELEASE:?}"
ROOT=/rootfs

apk add --no-cache -q cpio e2fsprogs kmod

log "installing packages: ${PACKAGES}"
mkdir -p "${ROOT}"
# shellcheck disable=SC2086 # word splitting of the package list is intended
apk add -q --root "${ROOT}" --initdb --no-cache --no-scripts \
    --keys-dir /etc/apk/keys --repositories-file /etc/apk/repositories ${PACKAGES}
apk info -v --root "${ROOT}" 2>/dev/null | sort > /out/manifest.txt

# Package scripts were skipped (they would need to run on the target), so the busybox
# applet links are created here. Existing files from other packages (kmod's modprobe,
# for example) are kept because "--install" does not overwrite.
mkdir -p "${ROOT}/bin" "${ROOT}/sbin" "${ROOT}/usr/bin" "${ROOT}/usr/sbin"
ln -sf busybox.static "${ROOT}/bin/busybox"
for applet in $(chroot "${ROOT}" /bin/busybox.static --list-full); do
    [ -e "${ROOT}/${applet}" ] || [ -L "${ROOT}/${applet}" ] || ln -s /bin/busybox "${ROOT}/${applet}"
done
ln -sf sbin/init "${ROOT}/init"

# Trim what a test image never needs.
rm -rf "${ROOT}/lib/apk" "${ROOT}/var/cache/apk" "${ROOT}/etc/apk" \
       "${ROOT}/usr/share/man" "${ROOT}/usr/share/doc" "${ROOT}/usr/share/info"

log "adding kernel modules ${KERNEL_RELEASE} and galcore"
tar -xzf "/boot-out/modules-${KERNEL_RELEASE}.tar.gz" -C "${ROOT}"
install -D -m 644 /boot-out/galcore.ko "${ROOT}/lib/modules/${KERNEL_RELEASE}/extra/galcore.ko"
depmod -b "${ROOT}" "${KERNEL_RELEASE}"

log "adding overlay"
cp -a /repo/Emulator/testimage/rootfs/. "${ROOT}/"
chmod 755 "${ROOT}/etc/init.d/rcS" "${ROOT}/usr/bin/kick-selftest"
mkdir -p "${ROOT}/proc" "${ROOT}/sys" "${ROOT}/dev" "${ROOT}/tmp" "${ROOT}/run" "${ROOT}/root"
# The kernel opens /dev/console before init runs and mounts devtmpfs.
[ -c "${ROOT}/dev/console" ] || mknod -m 600 "${ROOT}/dev/console" c 5 1
[ -c "${ROOT}/dev/null" ]    || mknod -m 666 "${ROOT}/dev/null" c 1 3

log "packing initramfs"
(cd "${ROOT}" && find . | LC_ALL=C sort | cpio --quiet -o -H newc -R 0:0) \
    | gzip -9 > /out/.initramfs.cpio.gz
mv -f /out/.initramfs.cpio.gz /out/initramfs.cpio.gz

log "creating rootfs.ext4 (${EXT4_SIZE})"
rm -f /out/.rootfs.ext4
mke2fs -q -t ext4 -L kick-test -d "${ROOT}" -E root_owner=0:0 /out/.rootfs.ext4 "${EXT4_SIZE}"
mv -f /out/.rootfs.ext4 /out/rootfs.ext4

log "rootfs content: $(du -sh "${ROOT}" | cut -f1)"
