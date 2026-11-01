# KICK boot chain, kernel and eMMC image

Builds the software the KICK runs before user space, from exactly the sources and
configuration the product build uses, so the emulator runs the real boot chain:

| Component | Source (read from `Linux/buildroot/configs/KTKick_defconfig`) |
|---|---|
| Kernel | varigit `linux-imx` (5.4.127) at `BR2_LINUX_KERNEL_CUSTOM_REPO_VERSION`, patches from `BR2_LINUX_KERNEL_PATCH`, config `BR2_LINUX_KERNEL_CUSTOM_CONFIG_FILE` (unmodified) |
| DTBs | `BR2_LINUX_KERNEL_INTREE_DTS_NAME` (the KICK uses `imx8mm-var-som-symphony.dtb`) |
| GPU module | `galcore.ko` from Buildroot `package/freescale-imx/kernel-module-imx-gpu-viv` (version and sha256 from its `.mk`/`.hash`) |
| ATF | varigit `imx-atf` at `BR2_TARGET_ARM_TRUSTED_FIRMWARE_CUSTOM_REPO_VERSION`, `PLAT=imx8mm`, BL31 |
| U-Boot | varigit `uboot-imx` at `BR2_TARGET_UBOOT_CUSTOM_REPO_VERSION`, patches from `BR2_TARGET_UBOOT_PATCH`, config `BR2_TARGET_UBOOT_CUSTOM_CONFIG_FILE` |
| imx-boot | Buildroot `package/imx-mkimage` (tag and patches), assembled by running the board's own `imx8-bootloader-prepare.sh` |
| eMMC layout | `board/freescale/imx8mm_var_som/genimage.cfg.emmc.partiton.table` |

Nothing above is duplicated in the scripts: URLs, commits, file names, padding sizes and
the post-image script are parsed from the defconfig and the Buildroot package files.
Paths, images, sizes and mirrors live in `Emulator/config/kickemu.json`.

## Requirements

- OrbStack or Docker (`docker` CLI). The scripts start OrbStack when the daemon is down.
- `jq` on the host (reads the configuration).
- Disk: about 1.5 GB for the Docker volume `kickemu-boot-build` (sources and objects),
  550 MB for the builder image, 40 MB in `Emulator/out/boot`, 50 MB for the test image
  and 72 MB actually used by the sparse 14 GiB `emmc.img`.

## Usage

```sh
# Everything: kernel, galcore, ATF, U-Boot, imx-boot
Emulator/boot/build.sh

# Single stages, in dependency order
Emulator/boot/build.sh kernel          # Image, Image.gz, DTBs, modules
Emulator/boot/build.sh gpu             # galcore.ko (needs kernel)
Emulator/boot/build.sh atf uboot       # bl31.bin, SPL, U-Boot
Emulator/boot/build.sh imxboot         # imx-boot (needs atf and uboot)
Emulator/boot/build.sh dtdoc           # decompiled DTS + node table for docs/board-dt.md

# Hardware-capable imx-boot with the NXP DDR training firmware (see below)
Emulator/boot/build.sh --ddr-fw ~/nxp/firmware-imx-8.10/firmware/ddr/synopsys imxboot

# Test image (Emulator/testimage), then the eMMC image with the KICK partition layout
Emulator/testimage/build.sh
Emulator/boot/mkimage-emmc.sh
```

Options of `build.sh`: `--config <file>`, `--out-dir <dir>`, `--ddr-fw <dir>`,
`--jobs <n>`, `--shell` (interactive shell in the builder with all mounts),
`--drop-work` (delete the build volume). `build.sh --help` prints the full list.

Options of `mkimage-emmc.sh`: `--rootfs <ext4 image>` (default
`.boot.emmc.rootfs_image`, the test image), `--imx-boot <file>`, `--output <file>`
(default `.storage.emmc_image`), `--size <size>` (default `.storage.emmc_size`, 14G),
`--rootfs-size <size>` (default `.boot.emmc.rootfs_size`, or
`BR2_TARGET_ROOTFS_EXT2_SIZE` = 762M when empty).

A first full build takes about 10 minutes on an M4 Pro (most of it the kernel fetch and
build); stages skip fetching and patching when the pinned source is already present.

## Outputs

Relative paths in the configuration resolve against the **main checkout** of the
repository (not the git worktree a script runs from), so every engineer finds the
artifacts in `/path/to/0700-AAM/Emulator/out/boot`. Files are published atomically, the
kernel stage publishes `Image` and the DTBs before it builds modules.

| File | Content |
|---|---|
| `Image`, `Image.gz` | arm64 kernel (`Image.gz` is what the KICK boots) |
| `imx8mm-var-som-symphony*.dtb` | the four DTBs from the defconfig |
| `modules-<release>.tar.gz`, `kernel.release` | stripped modules as `lib/modules/<release>` |
| `kernel.config`, `System.map` | final kernel configuration and symbols |
| `galcore.ko` | Vivante GPU kernel module for this kernel |
| `bl31.bin`, `bl31.elf` | ATF BL31 (load address 0x920000) |
| `u-boot-spl.bin`, `u-boot-nodtb.bin`, `u-boot.dtb`, `u-boot.bin` (nodtb + dtb), `uboot.config` | U-Boot build |
| `u-boot-spl.elf`, `u-boot.elf` | the same with symbols, for gdb |
| `u-boot-spl-ddr.bin` | SPL + DDR training blobs (loaded by the BootROM to 0x7e1000) |
| `u-boot.itb` | FIT with U-Boot, BL31 and the two U-Boot DTBs (loaded by SPL) |
| `imx-boot` | the boot image as written to eMMC/SD at 33 KiB (`imx8-boot-sd.bin`) |
| `imx-boot.info` | which DDR firmware went into `imx-boot` (`nxp` or `zero`) |
| `board-dt-table.md`, `imx8mm-var-som-symphony.dts` | from the `dtdoc` stage |

`mkimage-emmc.sh` writes `Emulator/out/images/emmc.img`: a sparse 14 GiB raw image with
the KICK GPT (disk GUID `8da16a96-64cb-44ef-9bd7-43cea57d091c`, entries at 7 MiB, no
backup GPT), `imx-boot` at 33 KiB outside the partition table, partition 1 `rootfs`
(PARTUUID `70f29e13-761e-461d-8a65-991d765941b7`) at 8 MiB with the given ext4 image,
and partition 2 `usr` (the KICK storage partition, PARTUUID
`56d2e551-db06-46da-bb58-274c6ee725d2`) over the rest of the device, left empty: the KICK
formats it on first boot (`kick-storage-mkfs.service`). The rootfs file system keeps the
size of the given image; grow it with `resize2fs` inside the guest if needed.

## DDR training firmware (NXP EULA)

The i.MX8MM SPL loads the LPDDR4/DDR4 PHY training firmware from NXP's `firmware-imx`
package. That package is licensed under the NXP EULA and is **not** downloaded or accepted
by these scripts.

- **With the firmware:** accept the EULA yourself, extract `firmware-imx-8.10.bin`, and
  pass the directory with `lpddr4_pmu_train_{1d,2d}_{imem,dmem}.bin` and
  `ddr4_{imem,dmem}_{1d,2d}_201810.bin` via `--ddr-fw <dir>` or
  `boot.ddr_firmware_dir` in the configuration. The blobs are padded and concatenated
  exactly as Buildroot's `firmware-imx.mk` does (IMEM 0x8000, DMEM 0x1000 from the
  defconfig; LPDDR4 1D, 2D, then DDR4 1D, 2D, which is why U-Boot has
  `CONFIG_IMX8M_DDRPHY_FW_OFFSET=0x12000`). The result can boot real hardware.
- **Without it (default):** the same layout is filled with zeros. The image is identical
  in structure and size, the build prints a warning and `imx-boot.info` says
  `ddr_firmware=zero`. **Such an imx-boot is only valid for the emulator**, whose DDR PHY
  model does not execute training firmware. Never write it to a real KICK.

## Toolchain and differences to the product build

- Builder: Debian bullseye arm64 (native on Apple Silicon), GCC 10.2.1. The defconfig uses
  Buildroot 2021.02's external Arm GNU-A 10.2-2020.11 toolchain, which Arm only ships for
  x86_64 hosts; bullseye's GCC is the same 10.2 release built natively for arm64, so no
  x86 emulation is needed. Kernel, ATF, U-Boot and galcore are built natively
  (`CROSS_COMPILE` empty). No source was patched for the compiler.
- bullseye now lives on `archive.debian.org`; the mirrors are configurable
  (`boot.builder.apt_mirror`, `apt_security_mirror`).
- Sources moved since Buildroot 2021.02 are mapped by `boot.url_rewrites`
  (`git://github.com/` -> `https://github.com/`, `source.codeaurora.org/external/imx/`
  -> `github.com/nxp-imx/`).
- The kernel config is used as is. Buildroot additionally applies config fixups for the
  product (custom boot logo `Logo.png` via `BR2_LINUX_KERNEL_CUSTOM_LOGO_PATH`, and the
  fixups of enabled packages such as systemd); those are not applied here, so the boot
  logo is the default one.
- U-Boot's own `tools/mkimage` from the pinned tree stands in for Buildroot's
  `host-uboot-tools`; genimage is version 14 as pinned by Buildroot.

## How it works

- `build.sh` (host) builds the builder image from `Dockerfile`, creates the Docker volume
  and runs `container/build.sh` with the checkout mounted read-only at `/repo`, the
  configuration at `/cfg/kickemu.json`, the output directory at `/out` and the volume at
  `/work`. The volume is used for sources and objects because the kernel tree needs a
  case-sensitive file system.
- `container/stage-*.sh` hold one stage each; `container/lib.sh` has the fetch (shallow,
  pinned commit, `.git` removed so the kernel version has no git suffix), patch (same
  `patch` options as Buildroot's `apply-patches.sh`) and publish helpers;
  `lib/common.sh` reads the configuration and the Buildroot files.
- `stage-imxboot.sh` recreates the Buildroot post-image environment (`BR2_CONFIG`,
  `BINARIES_DIR`, `HOST_DIR/bin` with `mkimage_imx8`, `mkimage_fit_atf.sh`,
  `imx8m_pad_image.sh`, `mkimage`) and runs the board's `imx8-bootloader-prepare.sh`.
- `mkimage-emmc.sh` renders the genimage template with the same substitutions as
  `post-image.sh` (its `imx_offset` helper is evaluated directly), sets the device size
  and puts the rootfs image into partition 1.
- `tools/dt-summary.py` turns a DTB into the Markdown node table in
  `Emulator/docs/board-dt.md`.

Clean up with `Emulator/boot/build.sh --drop-work` (removes the volume) and by deleting
`Emulator/out/boot`, `Emulator/out/testimage` and `Emulator/out/images`.
