#!/bin/sh
#
# BR2_ROOTFS_POST_BUILD_SCRIPT of the emulator variant (emulator.fragment). Runs after the
# rootfs overlays are copied, with the target directory as $1.
#
# 1. The board overlay keeps its .wants entries as copies of the units. systemd ignores
#    those ("is not a symlink, ignoring"), so the KICK storage partition would never be
#    mounted. Only media-kickstorage.mount is linked: the /etc and /var overlay mounts and
#    the /dev/mmcblk1p* mounts stay inactive, as in Emulator/rootfs (kick-app profile).
# 2. ktkick.service (replaced by Emulator/firmware/overlay) is enabled; udev also pulls it
#    in when fb0 appears (71-kick-x.rules).
# 3. The directories KT_Kick uses at run time exist (Kick/modules/common kick_paths.cpp and
#    the hardware helpers); /app receives the KT_Kick data when the image is assembled.

set -eu

TARGET="$1"
SYSTEMD="${TARGET}/etc/systemd/system"

wants="${SYSTEMD}/local-fs.target.wants/media-kickstorage.mount"
if [ -f "${wants}" ] && [ ! -L "${wants}" ]; then
    ln -sf ../media-kickstorage.mount "${wants}"
fi

mkdir -p "${SYSTEMD}/multi-user.target.wants"
ln -sf ../ktkick.service "${SYSTEMD}/multi-user.target.wants/ktkick.service"

chmod 0755 "${TARGET}/usr/bin/kt_kick.sh" "${TARGET}/usr/bin/kick-storage-mkfs"

mkdir -p "${TARGET}/home/Kick/projects" "${TARGET}/home/Kick/logs" "${TARGET}/home/Kick/prefs" \
    "${TARGET}/data/sounds" "${TARGET}/mnt/flash" "${TARGET}/mnt/sdcard" "${TARGET}/app"
