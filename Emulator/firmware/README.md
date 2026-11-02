# KICK Buildroot image for KickEmu

The `firmware-image` workflow (`.github/workflows/firmware-image.yml`) builds a root file
system, kernel and eMMC image for the emulated KICK from the repository's own Buildroot tree
(`Linux/buildroot`, Buildroot 2021.02.3, `configs/KTKick_defconfig`), with KT_Kick
cross-built against it. It runs on GitHub's runners; nothing is built locally. KickEmu boots
the result with `--profile buildroot`.

## Getting an image

The workflow starts on pushes that change its inputs (the workflow file, `Emulator/firmware/`,
`Linux/buildroot/`, `Kick/`) and by hand:

```sh
gh workflow run firmware-image.yml --ref <branch>
gh run watch                       # pick the run
gh run download <run id> -n kick-firmware -D Emulator/out/firmware
cd Emulator/out/firmware && sha256sum -c SHA256SUMS && zstd -d --rm kick-emmc.img.zst
```

The artifact `kick-firmware` is kept 14 days. It holds `kick-emmc.img.zst` (14 GiB raw eMMC
image, sparse after `zstd -d`), `Image`, `imx8mm-var-som-symphony.dtb`, `manifest.txt` (kernel,
KT_Kick commit, image checksum) and `SHA256SUMS`. It is never attached to a release.

```sh
KickEmu=Emulator/out/host/KickEmu_artefacts/Release/KickEmu.app/Contents/MacOS/KickEmu
$KickEmu --profile buildroot
```

The kernel boots with `root=/dev/mmcblk2p1`; the serial console (ttymxc3) shows a login prompt
(`root`, no password). KT_Kick appears on the panel after X starts and the KICK's start
script, `kt_kick.sh`, has waited its 15 seconds.

A first build takes several hours (the Buildroot job alone has a 350 minute limit). Each job
caches its result keyed on its inputs, so a change to the app or image stage reuses the
Buildroot output, and downloads are cached between runs.

## What is built

| Job | Where | Result |
|---|---|---|
| Buildroot | `debian:bullseye` container on `ubuntu-24.04` (2021.02.3 host packages do not build with GCC 13) | `rootfs.ext4`, `Image`, the DTB, the SDK (toolchain + sysroot) |
| KT_Kick | `ubuntu-24.04`, SDK toolchain (`app/toolchain.cmake` wraps Buildroot's `toolchainfile.cmake`) | `KT_Kick` + `assets/`, `templates/`, `demos/` |
| eMMC image | `ubuntu-24.04` | KT_Kick in `/usr/bin` and `/app`, GPT from the board's `genimage.cfg.emmc.partiton.table` |

The configuration is `KTKick_defconfig` plus `buildroot/emulator.fragment`, then
`olddefconfig`. Kconfig drops options whose dependencies fail without saying so, so
`buildroot/check-config.py` compares every line of the defconfig and the fragment with the
resolved `.config` and fails the build on any difference; every option the emulator variant
removes is therefore written out in the fragment.

The repository's copy of Buildroot has lost the executable bits of its scripts; the build
works on a copy and restores them for files that start with `#!`. Two download locations
moved since 2021 and are rewritten for git: `source.codeaurora.org/external/imx` (libdrm-imx)
to `github.com/nxp-imx`, and `git://github.com` to `https://`. The kernel (varigit
`linux-imx` at the pinned commit) is fetched as GitHub's archive of that commit instead of a
full clone.

KT_Kick comes from [KickEm-Tools/0700-AAM](https://github.com/KickEm-Tools/0700-AAM) at the
commit in the workflow's `KTKICK_REF`, not from this repository's `Kick/`: the public release
does not configure, because `AudIO/juce_modules.cmake` and the `AudIO_juce` and
`AudIO_constants` modules that Kick includes are missing (the fork restores them; it is also
what `Emulator/rootfs` uses). JUCE 7.0.12 is fetched and patched by Kick's CMake as usual.

## What is left out, and why

These parts of `KTKick_defconfig` are NXP software under the NXP EULA, which does not allow
publishing it in a public image:

| Left out | Effect in the emulator |
|---|---|
| `firmware-imx` (SDMA, VPU, DDR training firmware) | none: the emulated SDMA works without the firmware |
| `imx-gpu-viv`, `imx-gpu-g2d` (Vivante GLES/EGL, G2D) | no GPU userspace; KT_Kick renders in software (JUCE, no OpenGL) |
| `imx-codec`, `imx-parser` | none for KT_Kick |
| U-Boot, ATF, `imx-mkimage`, the post-image scripts | QEMU loads `Image` and the DTB directly; imx-boot would need the NXP DDR firmware |

Consequences that Kconfig forces and the fragment states: Weston's default backend becomes
fbdev (DRM needs an EGL provider), and libepoxy, Weston's XWayland and the Weston demo clients
are dropped (they need EGL/GLES). The Vivante kernel module (`galcore`, GPL-2.0) is kept.

Additions: `xinit` (starts X), `e2fsprogs` (`kick-storage-mkfs` from the board overlay runs
`e2label` and `mkfs.ext4`), and the libraries KT_Kick links or loads (ALSA, freetype,
fontconfig, the X client libraries, libXi for the multitouch patch).

## How it starts, compared with the KICK

| | KICK | This image |
|---|---|---|
| Display | udev starts `weston@root` (DRM, G2D) for fb0/card0; KT_Kick is an X client of Xwayland on `:0` | udev starts `ktkick.service` for fb0 (`overlay/etc/udev/rules.d/71-kick-x.rules`): `xinit /bin/sh /usr/bin/kt_kick.sh -- Xorg :0 vt7` with the fbdev driver on `/dev/fb0`; the board's `71-weston-drm.rules` is replaced by an empty rule |
| App | `ktkick.service` runs `kt_kick.sh` | the same script, as X's client; same resource limits |
| Storage | `.wants` entries are copies, which systemd ignores | `media-kickstorage.mount` is linked (as in `Emulator/rootfs`); `/etc`, `/var` overlays and the `mmcblk1p*` mounts stay inactive |
| Console | getty on ttymxc3 | the same; `kernel.printk = 3 4 1 7` (driver messages from interrupt context slow the emulated console) |

Files: `buildroot/post-build.sh` (links, `ktkick.service` enabled, app directories) and
`overlay/` (units, udev rules, X configuration).

## Licenses

Buildroot packages keep their own licenses. KT_Kick and this repository are AGPL-3.0; JUCE
is used under its AGPLv3 option. `manifest.txt` names the KT_Kick commit and the kernel.
