#!/bin/sh
#
# Runs inside alpine (arm64): builds an Alpine root file system with Xfce on the KICK
# framebuffer and writes /out/rootfs.ext4 and /out/emmc-desktop.img (GPT, rootfs on p1,
# so root=/dev/mmcblk2p1 as on the KICK).
#
# Environment: PACKAGES, KERNEL_RELEASE, ROOTFS_SIZE, EMMC_SIZE, ROOT_PASSWORD, HOSTNAME_

set -eu

R=/rootfs
apk add --no-cache e2fsprogs sfdisk coreutils >/dev/null

mkdir -p "$R/etc/apk"
cp -a /etc/apk/repositories /etc/apk/keys "$R/etc/apk/"
# shellcheck disable=SC2086
# The container is arm64 like the target, so package scripts and triggers run as is.
apk add --root "$R" --initdb --no-cache alpine-base $PACKAGES >/dev/null

# Vendor kernel modules (USB HID, USB storage, ...): eudev loads them on hotplug.
tar -xzf "/boot-out/modules-${KERNEL_RELEASE}.tar.gz" -C "$R"
chroot "$R" depmod "$KERNEL_RELEASE" || true

cat > "$R/etc/fstab" <<'EOF'
/dev/mmcblk2p1  /     ext4  rw,noatime  0 1
tmpfs           /tmp  tmpfs nosuid,nodev  0 0
EOF
echo "$HOSTNAME_" > "$R/etc/hostname"
printf 'auto lo\niface lo inet loopback\nauto eth0\niface eth0 inet dhcp\n' > "$R/etc/network/interfaces"

# Serial console on UART4 (the KickEmu console), root login with a known password.
sed -i 's/^tty[2-6]/#&/' "$R/etc/inittab"
grep -q ttymxc3 "$R/etc/inittab" || echo 'ttymxc3::respawn:/sbin/getty -L 115200 ttymxc3 vt100' >> "$R/etc/inittab"
echo ttymxc3 >> "$R/etc/securetty"
echo "root:${ROOT_PASSWORD}" | chroot "$R" chpasswd

# Desktop user that X runs as.
chroot "$R" adduser -D -s /bin/sh kick
echo "kick:${ROOT_PASSWORD}" | chroot "$R" chpasswd
for g in video input audio tty; do chroot "$R" addgroup kick "$g" 2>/dev/null || true; done

for s in devfs dmesg mdev; do rm -f "$R/etc/runlevels/sysinit/$s"; done
for s in udev udev-trigger udev-settle; do ln -sf "/etc/init.d/$s" "$R/etc/runlevels/sysinit/$s"; done
for s in dbus networking; do ln -sf "/etc/init.d/$s" "$R/etc/runlevels/default/$s"; done
ln -sf /etc/init.d/local "$R/etc/runlevels/default/local"

# X on fb0 (the eLCDIF scan-out), touch via evdev; no GLX (software rendering only).
mkdir -p "$R/etc/X11/xorg.conf.d"
cat > "$R/etc/X11/xorg.conf.d/10-kick.conf" <<'EOF'
Section "ServerFlags"
    Option "BlankTime" "0"
    Option "StandbyTime" "0"
    Option "SuspendTime" "0"
    Option "OffTime" "0"
EndSection
Section "Extensions"
    Option "GLX" "Disable"
EndSection
Section "Device"
    Identifier "kick-fb"
    Driver "fbdev"
    Option "fbdev" "/dev/fb0"
EndSection
Section "Screen"
    Identifier "kick-screen"
    Device "kick-fb"
EndSection
Section "InputClass"
    Identifier "kick-touch"
    MatchProduct "Goodix Capacitive TouchScreen"
    MatchDevicePath "/dev/input/event*"
    Driver "evdev"
EndSection
EOF

# The session starts from an init script, not a login on a VT: let it run X.
printf 'allowed_users=anybody\nneeds_root_rights=yes\n' > "$R/etc/X11/Xwrapper.config"

# Start Xfce as the desktop user once the system is up.
cat > "$R/etc/local.d/desktop.start" <<'EOF'
#!/bin/sh
( sleep 2; su -l kick -c 'startx /usr/bin/startxfce4 -- :0 vt7 -nolisten tcp -s 0 -dpms' > /var/log/desktop.log 2>&1 ) &
EOF
chmod +x "$R/etc/local.d/desktop.start"
echo 'kick-desktop: Xfce starting' > "$R/etc/motd"

# Root file system image, then the eMMC: GPT, 16 MiB gap like the KICK, rootfs as p1.
rm -f /out/rootfs.ext4 /out/emmc-desktop.img
mkfs.ext4 -q -L kick-desktop -d "$R" /out/rootfs.ext4 "$ROOTFS_SIZE"
truncate -s "$EMMC_SIZE" /out/emmc-desktop.img
printf 'label: gpt\nstart=32768, size=%s, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name=rootfs\n' \
    "$(( $(stat -c %s /out/rootfs.ext4) / 512 ))" | sfdisk -q /out/emmc-desktop.img
dd if=/out/rootfs.ext4 of=/out/emmc-desktop.img bs=512 seek=32768 conv=notrunc,sparse status=none
rm -f /out/rootfs.ext4   # it lives on in p1
du -h --apparent-size /out/emmc-desktop.img
du -h /out/emmc-desktop.img
