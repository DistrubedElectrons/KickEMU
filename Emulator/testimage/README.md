# KICK hardware test image

A small aarch64 root file system to validate the emulated hardware through the real KICK
kernel drivers before any firmware image is used.

```sh
Emulator/boot/build.sh kernel gpu      # kernel modules and galcore are taken from here
Emulator/testimage/build.sh            # -> Emulator/out/testimage/
```

Outputs (`.testimage.out_dir`, relative to the main checkout):

- `initramfs.cpio.gz`: for `-initrd` with `boot=kernel`
- `rootfs.ext4`: the same tree as an ext4 image (`.testimage.rootfs_ext4_size`), used by
  `Emulator/boot/mkimage-emmc.sh` for the eMMC rootfs partition
- `manifest.txt`: Alpine package versions

Content: Alpine (`.testimage.base_image`) packages from `.testimage.packages`
(static busybox, evtest, i2c-tools, alsa-utils, mmc-utils, memtester, ethtool, devmem2,
kmod, modetest from libdrm-tests; `fbset` is a busybox applet), the kernel modules and
`galcore.ko` from the boot build, and the files under `rootfs/`.

## Start-up

`/init` and `/sbin/init` are busybox init. `/etc/init.d/rcS` mounts proc, sysfs,
devtmpfs, devpts, tmpfs and debugfs, sets the host name and loads the modules for all
devices found (modalias coldplug, twice so that devices created by modules are covered).
A shell runs on the console `ttymxc3`.

Kernel command line options:

- `kick.selftest=1`: run `kick-selftest` after start
- `kick.poweroff=1`: power off after the self-test (automated runs)

## kick-selftest

`kick-selftest [-c <conf>] [item...]` prints one `PASS`/`FAIL` line per check and ends
with `KICK-SELFTEST: <n> PASS, <m> FAIL`; the exit status is the number of failures.
Expected values (controller addresses, I2C devices and drivers, audio format, panel mode,
timeouts) are in `/etc/kick-selftest.conf`.

| Item | Check |
|---|---|
| `soc` | `/sys/devices/soc0/soc_id` is `i.MX8MM`, prints family, revision, serial |
| `uarts` | ttymxc0..3 exist, sit on 0x30860000/0x30890000/0x30880000/0x30a60000, accept termios |
| `i2c` | i2c-0..3 on I2C1..4; BD71847 (0-004b), GT911 (2-0014) and RTC (2-0068) bound to their drivers; CV converter 0x0c on i2c-3 acknowledges |
| `cv` | DAC write (0 V) and ADC read on the CV converter, same transactions as `kick_I2c.cpp` |
| `rtc` | the I2C RTC reads with `hwclock` |
| `mmc` | eMMC `mmcblk2` on uSDHC3 and SD `mmcblk1` on uSDHC2 with partitions; EXT_CSD read with `mmc` |
| `eth` | eth0 carrier, link speed via ethtool, DHCP lease with udhcpc |
| `audio` | ALSA card0, 1 s playback and capture at S32_LE, 48 kHz, 2 channels on `hw:0,0` |
| `touch` | GT911 input device; waits `TOUCH_EVENT_TIMEOUT` s for touch events (0 = presence only) |
| `display` | `/dev/dri/card0` with a connected connector, `/dev/fb0` geometry equals `PANEL_MODE` |
| `gpu` | `galcore` loads and `/dev/galcore` appears |
| `pwrkey` | SNVS power key input device |
| `memory` | memtester over `MEMTEST_SIZE`, one loop |

The expected display mode is 1024x600 because the board DT sets those timings on the
SN65DSI84 bridge (see `Emulator/docs/board-dt.md`). The RTC node is `dallas,ds1337`
although the board has a PCF85263; the check reports what the kernel driver sees.
