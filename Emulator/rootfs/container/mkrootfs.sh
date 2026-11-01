#!/usr/bin/env bash
#
# Runs inside the debian:bookworm stage container (started by Emulator/rootfs/build.sh).
# Turns the container itself into the KICK root file system: Debian packages, the vendor
# kernel modules and galcore, KT_Kick with its /app data, the KICK service files from the
# Buildroot board overlay, and Emulator/rootfs/overlay. The host exports the container
# afterwards (container/pack.sh), so everything here works on "/".
#
# Environment: see stage_rootfs in Emulator/rootfs/lib/stage.sh.
# Mounts: /rootfs-src (Emulator/rootfs), /board (board rootfs_overlay), /boot-out (boot
# build outputs), /app-pkg (KT_Kick package), /out (build output directory).

set -euo pipefail

log() { echo "[rootfs] $*" >&2; }

: "${PACKAGES:?}" "${KERNEL_RELEASE:?}" "${KICK_HOSTNAME:?}" "${CONSOLE_TTY:?}" "${APP_PACKAGE:?}"
: "${CONSOLE_LOGLEVEL:?}"
: "${X_DISPLAY:?}" "${X_VT:?}" "${X_FBDEV:?}" "${X_FB_KERNEL:?}" "${X_TOUCH_PRODUCT:?}"
export DEBIAN_FRONTEND=noninteractive

# --- packages -----------------------------------------------------------------------------
if [[ -n "${APT_MIRROR:-}" ]]; then
    sed -i "s|http://deb.debian.org/debian|${APT_MIRROR}|" /etc/apt/sources.list.d/debian.sources
fi
# No documentation, man pages or translations in the image.
cat > /etc/dpkg/dpkg.cfg.d/kick-nodoc <<'EOF'
path-exclude=/usr/share/doc/*
path-include=/usr/share/doc/*/copyright
path-exclude=/usr/share/man/*
path-exclude=/usr/share/info/*
path-exclude=/usr/share/locale/*
path-include=/usr/share/locale/locale.alias
EOF
log "installing packages: ${PACKAGES}"
apt-get update -qq
# shellcheck disable=SC2086 # the package list is split on purpose
apt-get install -y -qq --no-install-recommends ${PACKAGES} >/dev/null
dpkg-query -W -f '${Package} ${Version}\n' | sort > /out/packages.txt

# --- kernel modules -----------------------------------------------------------------------
log "adding kernel modules ${KERNEL_RELEASE}"
tar -xzf "/boot-out/modules-${KERNEL_RELEASE}.tar.gz" -C /usr
if [[ -f /boot-out/galcore.ko ]]; then
    install -D -m 644 /boot-out/galcore.ko "/usr/lib/modules/${KERNEL_RELEASE}/extra/galcore.ko"
fi
depmod "${KERNEL_RELEASE}"

# --- KT_Kick ------------------------------------------------------------------------------
# Install layout of the fork's CI package: /usr/bin/KT_Kick, /app/{assets,templates,demos}.
log "installing KT_Kick from ${APP_PACKAGE}"
pkg="$(mktemp -d)"
tar -xzf "/app-pkg/${APP_PACKAGE}" -C "${pkg}" --no-same-owner
top="$(find "${pkg}" -mindepth 1 -maxdepth 1 -type d | head -n 1)"
install -m 0755 "${top}/KT_Kick" /usr/bin/KT_Kick
mkdir -p /app
cp -R "${top}/assets" "${top}/templates" "${top}/demos" /app/
[[ -f "${top}/manifest.json" ]] && install -m 0644 "${top}/manifest.json" /app/manifest.json
rm -rf "${pkg}"
if ldd /usr/bin/KT_Kick | grep 'not found'; then
    echo "KT_Kick needs libraries that are not installed" >&2
    exit 1
fi
# Paths the app uses at run time (kick_paths.cpp and the hardware helpers).
mkdir -p /home/Kick/projects /home/Kick/logs /home/Kick/prefs /data/sounds /mnt/flash /mnt/sdcard

# --- KICK board overlay files -------------------------------------------------------------
log "copying files from the board overlay"
while IFS= read -r f; do
    [[ -n "${f}" ]] || continue
    [[ -e "/board/${f}" || -L "/board/${f}" ]] || { echo "board overlay has no ${f}" >&2; exit 1; }
    mkdir -p "/$(dirname "${f}")"
    cp -P "/board/${f}" "/${f}"
done <<<"${BOARD_FILES}"
# The board overlay keeps .wants entries as copies of the unit; systemd ignores those
# ("is not a symlink, ignoring"), so the storage partition would never be mounted.
find /etc/systemd/system -path '*.wants/*' -type f | while IFS= read -r w; do
    unit="/etc/systemd/system/$(basename "${w}")"
    [[ -f "${unit}" ]] || continue
    ln -sf "${unit}" "${w}"
done
[[ -f /usr/bin/kt_kick.sh ]] && chmod 0755 /usr/bin/kt_kick.sh
[[ -f /usr/bin/kick-storage-mkfs ]] && chmod 0755 /usr/bin/kick-storage-mkfs

# --- emulator overlay ---------------------------------------------------------------------
log "copying Emulator/rootfs/overlay"
cp -R /rootfs-src/overlay/. /

# Generated from the configuration.
printf '%s\n' "${APP_ENV}" > /etc/default/ktkick
cat > /etc/default/kick-x <<EOF
# X server for KT_Kick (ktkick.service). Written by Emulator/rootfs/build.sh.
KICK_X_DISPLAY=${X_DISPLAY}
KICK_X_VT=${X_VT}
KICK_X_ARGS=${X_ARGS}
EOF
# Console log level: driver messages from interrupt context (SAI xruns) are written to the
# serial console synchronously and slow the whole system down while they last.
cat > /etc/sysctl.d/10-kick-console.conf <<EOF
kernel.printk = ${CONSOLE_LOGLEVEL} 4 1 7
EOF
mkdir -p /etc/X11/xorg.conf.d
sed -e "s|@FBDEV@|${X_FBDEV}|" -e "s|@TOUCH_PRODUCT@|${X_TOUCH_PRODUCT}|" \
    /rootfs-src/templates/10-kick.conf.in > /etc/X11/xorg.conf.d/10-kick.conf
# As 71-weston-drm.rules on the KICK: the framebuffer pulls in the graphical session.
cat > /etc/udev/rules.d/71-kick-x.rules <<EOF
ACTION=="add", SUBSYSTEM=="graphics", KERNEL=="${X_FB_KERNEL}", TAG+="systemd", ENV{SYSTEMD_WANTS}+="ktkick.service"
EOF

# --- system settings ----------------------------------------------------------------------
# /etc/hostname, /etc/hosts and /etc/resolv.conf are docker's bind mounts here; pack.sh
# writes them after the export.
if [[ -n "${ROOT_PASSWORD:-}" ]]; then
    echo "root:${ROOT_PASSWORD}" | chpasswd
else
    passwd -d root >/dev/null
fi
if [[ -n "${AUTHORIZED_KEYS:-}" ]]; then
    install -d -m 0700 /root/.ssh
    printf '%s\n' "${AUTHORIZED_KEYS}" > /root/.ssh/authorized_keys
    chmod 0600 /root/.ssh/authorized_keys
fi
if [[ "${CONSOLE_AUTOLOGIN:-false}" == true ]]; then
    mkdir -p "/etc/systemd/system/serial-getty@${CONSOLE_TTY}.service.d"
    sed -e 's|@AUTOLOGIN@|--autologin root|' /rootfs-src/templates/serial-getty.conf.in \
        > "/etc/systemd/system/serial-getty@${CONSOLE_TTY}.service.d/kick.conf"
fi
# Host keys are made on the first boot (ssh.service drop-in), not shared by every image.
rm -f /etc/ssh/ssh_host_*

systemctl enable systemd-networkd.service systemd-resolved.service ssh.service ktkick.service
systemctl set-default multi-user.target
# Periodic maintenance an emulated unit does not need.
systemctl mask apt-daily.timer apt-daily-upgrade.timer e2scrub_all.timer e2scrub_reap.service \
    fstrim.timer 2>/dev/null || true

# --- trim ---------------------------------------------------------------------------------
apt-get clean
rm -rf /var/lib/apt/lists/* /var/cache/debconf/*-old /var/log/*.log /var/log/apt/*
: > /etc/machine-id
log "content: $(du -sxh / 2>/dev/null | cut -f1)"
