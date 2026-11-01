# Debian root file system for the KICK application

`Emulator/rootfs/build.sh` builds a Debian 12 (bookworm) arm64 root file system that runs the
KICK application, `KT_Kick`, on the emulated KICK, and writes it into rootfs partition 1 of an
eMMC image with the real KICK partition table. It uses prebuilt Debian packages: no Buildroot
and no cross toolchain. KickEmu boots it with `--profile kick-app`.

## What is in the image

| Part | Where it comes from |
|---|---|
| Debian 12 base, systemd, udev, kmod, iproute2, openssh-server, e2fsprogs, alsa-utils, i2c-tools, evtest | `debian:bookworm` (`.rootfs.packages`) |
| Xorg with the fbdev video and evdev input drivers, xinit, the X libraries JUCE loads at run time, DejaVu fonts | Debian packages |
| Vendor kernel modules 5.4.127 and `galcore.ko` | `Emulator/boot/build.sh` outputs (`.boot.out_dir`) |
| `/usr/bin/KT_Kick`, `/app/{assets,templates,demos}`, `/app/manifest.json` | the `KT_Kick-linux-arm64` package (below) |
| `asound.conf`, `kt_kick.sh`, `kick-storage-mkfs`, storage partition units, `10-imx.rules` | the KICK Buildroot board overlay (`.rootfs.board_overlay_files`) |
| X, network, ssh, journal, console settings | `Emulator/rootfs/overlay` and `templates/` |

The image is about 545 MiB in the 762 MiB rootfs partition of the KICK layout
(`.rootfs.image_size`, the size of `BR2_TARGET_ROOTFS_EXT2_SIZE`). Mesa and LLVM come in
as dependencies of `xserver-xorg-core`; GLX is disabled, so they are never loaded.

## Building

Requires the boot build (`Emulator/boot/build.sh kernel gpu imxboot`), a container runtime
and, for the default app source, the GitHub CLI (`gh`) logged in.

```sh
Emulator/rootfs/build.sh                  # app, rootfs, emmc
Emulator/rootfs/build.sh app              # only fetch or build KT_Kick
Emulator/rootfs/build.sh rootfs emmc      # rebuild the image with the package already there
Emulator/rootfs/build.sh --package KT_Kick-linux-arm64.tar.gz     # use a local package
Emulator/rootfs/build.sh --app-source source app                  # build KT_Kick from source
```

Outputs (relative paths resolve against the main checkout, as for the other scripts):

| File | |
|---|---|
| `Emulator/out/rootfs/app/KT_Kick-linux-arm64.tar.gz`, `manifest.json` | the KT_Kick package |
| `Emulator/out/rootfs/rootfs.ext4` | the root file system |
| `Emulator/out/rootfs/packages.txt` | installed Debian packages and versions |
| `Emulator/out/images/emmc-kick-app.img` | 14 GiB sparse eMMC image (`.rootfs.emmc_image`) |

Steps:

1. **app** puts `KT_Kick-linux-arm64.tar.gz` (layout of the fork's CI package:
   `KT_Kick`, `assets/`, `templates/`, `demos/`, `manifest.json`) into `out/rootfs/app`.
   `.rootfs.app.source` picks how:
   - `artifact`: downloads the newest unexpired `KT_Kick-linux-arm64` artifact built from
     `.rootfs.app.ref` of `.rootfs.app.repo` (or the run in `.rootfs.app.run_id`) with
     `gh run download`. CI artifacts expire after 30 days.
   - `source`: clones the repository at the ref in a `debian:bookworm` arm64 container and
     builds the `Kick` target as the fork's `.github/workflows/build.yml` does (same
     packages, `-DCMAKE_BUILD_TYPE=Release -DTIMESTAMP=OFF`, one compiler per CPU and per
     `.rootfs.app.build_mem_per_job_mb`). Sources, the CPM cache (JUCE 7.0.12) and the build
     tree stay in the volume `.rootfs.app.build_volume`.
   - `file`: takes `--package` or `.rootfs.app.package_file`.
2. **rootfs** installs everything in a named `debian:bookworm` container
   (`container/mkrootfs.sh`), exports it with `docker export` and packs it with `mke2fs -d`
   in a throwaway container (`container/pack.sh`), which also writes the files docker
   manages inside a container (hostname, hosts, resolv.conf, `/dev`).
3. **emmc** runs `Emulator/boot/mkimage-emmc.sh` with `rootfs.ext4`, `imx-boot` and
   `.rootfs.emmc_image`: GPT with the KICK disk and partition UUIDs, rootfs in partition 1
   (PARTUUID 70f29e13-761e-461d-8a65-991d765941b7), storage partition 2 empty.

## Running

```sh
KickEmu=Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu
$KickEmu --profile kick-app
ssh -p 2222 root@127.0.0.1                # password: .rootfs.root_password ("kick")
```

The profile boots the vendor kernel with `root=/dev/mmcblk2p1` from the eMMC image; the
serial console on ttymxc3 logs in as root automatically (`.rootfs.console_autologin`).
Under HVF the KT_Kick home screen shows about 17 s after QEMU starts (26 s on the first
boot, which formats the storage partition).

## How it starts, compared with the KICK

| | KICK (Buildroot image) | This image |
|---|---|---|
| Init | systemd | systemd 252 |
| Display server | Weston on DRM (G2D), Xwayland on `:0`, started by udev for fb0/card0 (`weston@root`) | Xorg with the fbdev driver on `/dev/fb0`, started by udev for fb0 (`71-kick-x.rules`) |
| App | `ktkick.service` runs `kt_kick.sh` (sleeps 15 s, then `/usr/bin/KT_Kick`) as root | `ktkick.service` runs `xinit /usr/bin/KT_Kick -- Xorg :0 vt7 -nocursor ...` as root, same resource limits; `kt_kick.sh` is installed but not used |
| App environment | none | `DISPLAY=:0` plus `.rootfs.app.environment` in `/etc/default/ktkick` (for example `KICK_AUDIO_PROBE`) |
| Touch | libinput in Weston | evdev for `Goodix Capacitive TouchScreen` (800x480 axes scaled to 1024x600) |
| Audio | `asound.conf`: default = plug to card0 S32_LE 48 kHz | the same file |
| Storage | `kick-storage-mkfs` formats PARTUUID 56d2e551-... on first boot, mounted at `/media/kickstorage`, `/etc` and `/var` overlays on it | formatted and mounted the same way (with `noinit_itable`); no `/etc` and `/var` overlays |
| Network | the app runs `ifconfig`/`udhcpc` on eth0 | systemd-networkd DHCP on eth0 (kernel name kept by `10-kick-eth0.link`); `ifconfig` and `udhcpc` are not installed |
| Console | getty on ttymxc3 | serial getty on ttymxc3, autologin, `kernel.printk` from `.rootfs.console_loglevel` |
| sshd | no | yes, host keys made on first start |

Paths the app uses exist: `/home/Kick/{projects,logs,prefs}`, `/data/sounds`, `/mnt/flash`,
`/mnt/sdcard`, `/dev/ttymxc0` (MCU), `/dev/i2c-3` (CV DAC).

## What was checked (HVF, headless, QMP)

- Display: KT_Kick draws its Projects screen, Preferences and the project screens on fb0.
- Touch: QMP `input-send-event` abs + btn on the GT911 reaches the app at the right place.
- MCU: the handshake on ttymxc0 completes, panel keys work (KNOB_ENTER opens a project), the
  app sends LED, pad and slider frames.
- Audio: the app runs card0 at 48 kHz S32_LE (period 480, buffer 1920), full duplex; with
  `-audiodev wav` a test tone reaches the host file. The app plays silence at idle (the
  public repository has no samples).
- Network: DHCP from QEMU user networking, Preferences > Ethernet shows the address, ssh.
- CV: `/dev/i2c-3` answers at 0x0c, Preferences > CV is available.

## Known limits

- The fork's `KICK_*` test hooks are not set; add them in `.rootfs.app.environment`.
- No factory samples, metronome sounds or Myriad Pro font (not in the public sources).
- The app reads the power key from `/dev/input/event2`; on the emulated board that node is
  `gpio-keys` (event0 is the SNVS power key).
- The app's own network commands (`ifconfig`, `udhcpc`, `killall`) are not installed, so
  the Ethernet settings page shows the DHCP state but does not change it.

## Licenses

- KT_Kick is built from [KickEm-Tools/0700-AAM](https://github.com/KickEm-Tools/0700-AAM)
  and is licensed under AGPL-3.0 like this repository; `/app/manifest.json` in the image
  names the commit it was built from. JUCE is used under its AGPLv3 option.
- The Debian packages keep their own licenses; `/usr/share/doc/*/copyright` stays in the
  image and `out/rootfs/packages.txt` lists every package and version.
- The kernel modules are GPL-2.0 (vendor kernel 5.4.127, see `Emulator/boot`); `galcore` is
  the Vivante GPU driver from NXP's `kernel-module-imx-gpu-viv` (GPL-2.0).
