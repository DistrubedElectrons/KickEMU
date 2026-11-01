/*
 * Behringer KICK (BE 0700-AAM): Variscite VAR-SOM-MX8M-MINI on the KICK
 * carrier board
 *
 * SOM: i.MX8M Mini, LPDDR4 at 0x4000_0000, BD71847 PMIC on I2C1 (0x4b,
 * IRQ on GPIO2_IO08), AR8033 PHY at MDIO address 4, eMMC on uSDHC3, SD slot
 * on uSDHC2 (imx8mm-var-som.dtsi, imx8mm-var-som-symphony.dts with the KICK
 * patches from Linux/buildroot/board/freescale/imx8mm_var_som/patches).
 *
 * Carrier: UART1 control-surface MCU, UART3 MIDI, UART4 console, GT911
 * touch and PCF85263 RTC on I2C3, CV/gate converter on I2C4, SAI2 audio,
 * MIPI-DSI panel. The models for those are separate; this file wires them
 * when they are compiled in (see kick_board_wire_* below).
 *
 * Boot modes (machine property "boot-mode", docs/ARCHITECTURE.md):
 *   kernel  -kernel/-dtb/-initrd, PSCI over SMC provided by QEMU
 *   uboot   -bios u-boot.bin loaded at "uboot-addr", PSCI from QEMU
 *   rom     BootROM emulation: the boot image is read from the eMMC or SD
 *           card at 33 KiB, SPL runs from OCRAM at EL3 (TCG only)
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "chardev/char.h"
#include "qemu/timer.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "hw/arm/boot.h"
#include "hw/arm/machines-qom.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/qdev-properties.h"
#include "pca9534.h"
#include "sn65dsi83.h"
#include "hw/i2c/i2c.h"
#include "hw/sd/sd.h"
#include "net/net.h"
#include "system/block-backend.h"
#include "system/hvf.h"
#include "system/kvm.h"
#include "system/qtest.h"
#include "system/reset.h"
#include "system/runstate.h"
#include "system/system.h"
#include "target/arm/cpu.h"
#include <libfdt.h>

#include "ar8033_phy.h"
#include "bd71847.h"
#include "fsl-imx8mm.h"
#include "kick-board.h"
#include "kick-bootrom.h"

#define TYPE_KICK_MACHINE MACHINE_TYPE_NAME("kick")
OBJECT_DECLARE_SIMPLE_TYPE(KickMachineState, KICK_MACHINE)

typedef enum KickBootMode {
    KICK_BOOT_KERNEL,
    KICK_BOOT_UBOOT,
    KICK_BOOT_ROM,
} KickBootMode;

struct KickMachineState {
    MachineState parent_obj;

    FslImx8mmState soc;
    AR8033PhyState phy;
    struct arm_boot_info boot_info;

    Notifier powerdown;
    QEMUTimer *pwrkey_release;

    /* Machine properties */
    char *boot;
    char *emmc;
    char *sd;
    char *cv_chardev;
    uint64_t soc_uid;
    uint64_t uboot_addr;
    uint64_t emmc_boot_part_size;
    uint32_t pwrkey_press_ms;
    uint32_t panel_width;
    uint32_t panel_height;
    bool gt911_reset_wired;
    char *rom_boot_device;
};

/* Optional board device models, see kick-board.h */
static bool kick_type_available(const char *type)
{
    return object_class_by_name(type) != NULL;
}

static I2CBus *kick_i2c_bus(KickMachineState *s, int n)
{
    return s->soc.i2c[n - 1].parent_obj.bus;
}

static DeviceState *kick_gpio(KickMachineState *s, int n)
{
    return DEVICE(&s->soc.gpio[n - 1]);
}

/*
 * GT911 touch controller on I2C3 at 0x14. INT goes to GPIO5_IO04 (the
 * driver also drives that pin as an output to select the I2C address
 * during reset), RESET comes from GPIO5_IO03 (active low).
 */
static void kick_board_wire_gt911(KickMachineState *s)
{
    DeviceState *dev;

    if (!kick_type_available(KICK_TYPE_GT911)) {
        return;
    }
    dev = DEVICE(i2c_slave_create_simple(kick_i2c_bus(s, 3), KICK_TYPE_GT911,
                                         KICK_GT911_I2C_ADDR));
    qdev_connect_gpio_out_named(dev, "int", 0,
                                qdev_get_gpio_in(kick_gpio(s, 5), 4));
    qdev_connect_gpio_out(kick_gpio(s, 5), 4,
                          qdev_get_gpio_in_named(dev, "int-sense", 0));
    /*
     * The DT declares reset-gpios GPIO5_IO03 GPIO_ACTIVE_LOW, so goodix_reset()
     * ends with the pin driven low. A GT911 whose RSTB sat on that pin would
     * stay in reset, yet touch works on the device: the line does not reset
     * the controller on the KICK. It is only connected on request.
     */
    if (s->gt911_reset_wired) {
        qdev_connect_gpio_out(kick_gpio(s, 5), 3,
                              qdev_get_gpio_in_named(dev, "reset", 0));
    }
}

/* NXP PCF85263 RTC on I2C3 at 0x68 (no interrupt routed in the DT) */
static void kick_board_wire_pcf85263(KickMachineState *s)
{
    if (kick_type_available(KICK_TYPE_PCF85263)) {
        i2c_slave_create_simple(kick_i2c_bus(s, 3), KICK_TYPE_PCF85263,
                                KICK_PCF85263_I2C_ADDR);
    }
}

/* CV/gate converter on I2C4 at 0x0c, host side on the "cv-chardev" socket */
static void kick_board_wire_cv(KickMachineState *s)
{
    I2CSlave *dev;

    if (!kick_type_available(KICK_TYPE_CV)) {
        return;
    }
    dev = i2c_slave_new(KICK_TYPE_CV, KICK_CV_I2C_ADDR);
    if (s->cv_chardev) {
        Chardev *chr = qemu_chr_find(s->cv_chardev);

        if (!chr) {
            error_report("kick: cv-chardev '%s' not found", s->cv_chardev);
            exit(1);
        }
        qdev_prop_set_chr(DEVICE(dev), "chardev", chr);
    }
    i2c_slave_realize_and_unref(dev, kick_i2c_bus(s, 4), &error_fatal);
}

/* SAI2 to the V4220M codec: playback/capture through a QEMU audiodev */
static void kick_board_setup_sai2(KickMachineState *s)
{
    DeviceState *sai2 = fsl_imx8mm_get_optional(&s->soc, "sai2");
    const char *audiodev = MACHINE(s)->audiodev;

    if (sai2 && audiodev && object_property_find(OBJECT(sai2), "audiodev")) {
        qdev_prop_set_string(sai2, "audiodev", audiodev);
    }
}

/*
 * NXP PCA9534 GPIO expander on I2C2 at 0x20 (gpio-keys Back/Home/Menu on
 * pins 1..3, hogs on 4..7). Its open-drain INT goes to GPIO1_IO07, which
 * the DT uses as a falling-edge interrupt.
 */
static void kick_board_wire_expander(KickMachineState *s)
{
    DeviceState *dev;

    dev = DEVICE(i2c_slave_create_simple(kick_i2c_bus(s, 2), TYPE_PCA9534,
                                         KICK_PCA9534_I2C_ADDR));
    qdev_connect_gpio_out_named(dev, PCA9534_GPIO_INT, 0,
                                qdev_get_gpio_in(kick_gpio(s, 1), 7));
}

/*
 * TI SN65DSI84 DSI-to-LVDS bridge on I2C1 at 0x2c, the DSIM's DSI
 * peripheral; EN comes from GPIO2_IO11.
 */
static void kick_board_wire_bridge(KickMachineState *s)
{
    I2CSlave *br = i2c_slave_new(TYPE_SN65DSI83, KICK_SN65DSI84_I2C_ADDR);

    object_property_set_link(OBJECT(br), "dsi-host", OBJECT(&s->soc.dsim),
                             &error_abort);
    i2c_slave_realize_and_unref(br, kick_i2c_bus(s, 1), &error_fatal);
    qdev_connect_gpio_out(kick_gpio(s, 2), 11,
                          qdev_get_gpio_in_named(DEVICE(br), "enable", 0));
}

/*
 * The I2C1..4 SCL/SDA pads double as GPIO5_IO14..21 (SPL and Linux use
 * them for bus recovery). The SOM pulls the lines up, so read as GPIO the
 * idle bus is high.
 */
static void kick_board_i2c_pullups(KickMachineState *s)
{
    for (int pin = KICK_I2C_GPIO_FIRST; pin <= KICK_I2C_GPIO_LAST; pin++) {
        qemu_set_irq(qdev_get_gpio_in(kick_gpio(s, 5), pin), 1);
    }
}

/* BD71847 PMIC on I2C1, INTB (active low) on GPIO2_IO08 */
static void kick_board_wire_pmic(KickMachineState *s)
{
    DeviceState *pmic;

    pmic = DEVICE(i2c_slave_create_simple(kick_i2c_bus(s, 1), TYPE_BD71847,
                                          KICK_PMIC_I2C_ADDR));
    qdev_connect_gpio_out_named(pmic, BD71847_GPIO_IRQ, 0,
                                qdev_get_gpio_in(kick_gpio(s, 2), 8));
}

/* AR8033 PHY on the ENET1 MDIO bus */
static void kick_board_wire_phy(KickMachineState *s)
{
    object_initialize_child(OBJECT(s), "ethphy0", &s->phy, TYPE_AR8033_PHY);
    object_property_set_link(OBJECT(&s->phy), "enet", OBJECT(&s->soc.enet),
                             &error_abort);
    qdev_prop_set_uint64(DEVICE(&s->phy), "enet-base",
                         fsl_imx8mm_memmap[FSL_IMX8MM_ENET1].addr);
    qdev_prop_set_uint32(DEVICE(&s->phy), "phy-addr", KICK_ENET_PHY_ADDR);
    qdev_realize(DEVICE(&s->phy), NULL, &error_fatal);
}

/* eMMC on uSDHC3, SD card slot on uSDHC2, from -drive if=none,id=... */
static void kick_board_attach_card(KickMachineState *s, int usdhc,
                                   const char *drive, const char *type)
{
    BlockBackend *blk;
    DeviceState *card;

    if (!drive) {
        return;
    }
    blk = blk_by_name(drive);
    if (!blk) {
        error_report("kick: drive '%s' not found (use -drive if=none,id=%s)",
                     drive, drive);
        exit(1);
    }
    card = qdev_new(type);
    if (!strcmp(type, TYPE_EMMC)) {
        qdev_prop_set_uint64(card, "boot-partition-size",
                             s->emmc_boot_part_size);
    }
    qdev_prop_set_drive_err(card, "drive", blk, &error_fatal);
    qdev_realize_and_unref(card,
                           qdev_get_child_bus(DEVICE(&s->soc.usdhc[usdhc - 1]),
                                              "sd-bus"),
                           &error_fatal);
}

/*
 * The ON/OFF button on the SNVS: QEMU's system_powerdown presses it for
 * "power-key-press-ms" (KEY_POWER in Linux, KEY_STOP in the KICK firmware).
 */
static void kick_pwrkey_release(void *opaque)
{
    KickMachineState *s = opaque;

    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->soc.snvs),
                                        IMX8MM_SNVS_PWRKEY, 0), 0);
}

static void kick_powerdown_req(Notifier *n, void *opaque)
{
    KickMachineState *s = container_of(n, KickMachineState, powerdown);

    qemu_set_irq(qdev_get_gpio_in_named(DEVICE(&s->soc.snvs),
                                        IMX8MM_SNVS_PWRKEY, 0), 1);
    timer_mod(s->pwrkey_release, qemu_clock_get_ms(QEMU_CLOCK_VIRTUAL) +
                                 s->pwrkey_press_ms);
}

static void kick_modify_dtb(const struct arm_boot_info *info, void *fdt)
{
    int offset;

    /*
     * QEMU's PSCI CPU_SUSPEND only waits for an interrupt; the power-down
     * idle state in imx8mm.dtsi expects a resume through the entry point.
     */
    offset = fdt_node_offset_by_compatible(fdt, -1, "arm,cortex-a53");
    while (offset >= 0) {
        fdt_nop_property(fdt, offset, "cpu-idle-states");
        offset = fdt_node_offset_by_compatible(fdt, offset, "arm,cortex-a53");
    }

    /*
     * With a hardware accelerator the generic timer is the host's counter;
     * drop the fixed 8 MHz so Linux reads CNTFRQ_EL0 instead.
     */
    if (hvf_enabled() || kvm_enabled()) {
        offset = fdt_node_offset_by_compatible(fdt, -1, "arm,armv8-timer");
        while (offset >= 0) {
            fdt_nop_property(fdt, offset, "clock-frequency");
            offset = fdt_node_offset_by_compatible(fdt, offset,
                                                   "arm,armv8-timer");
        }
    }
}

static KickBootMode kick_boot_mode(KickMachineState *s)
{
    if (!s->boot || !strcmp(s->boot, "kernel")) {
        return KICK_BOOT_KERNEL;
    }
    if (!strcmp(s->boot, "uboot")) {
        return KICK_BOOT_UBOOT;
    }
    if (!strcmp(s->boot, "rom")) {
        return KICK_BOOT_ROM;
    }
    error_report("kick: boot-mode=%s unknown, use kernel, uboot or rom",
                 s->boot);
    exit(1);
}

static void kick_init(MachineState *machine)
{
    KickMachineState *s = KICK_MACHINE(machine);
    KickBootMode mode = kick_boot_mode(s);
    bool accel_hw = hvf_enabled() || kvm_enabled();
    Object *soc = OBJECT(&s->soc);

    if (machine->ram_size > FSL_IMX8MM_RAM_SIZE_MAX) {
        error_report("kick: RAM size above the i.MX8MM maximum of 4 GiB");
        exit(1);
    }
    if (mode == KICK_BOOT_ROM && accel_hw) {
        error_report("kick: boot-mode=rom runs SPL and ATF at EL3, needs TCG");
        exit(1);
    }

    /*
     * Exception levels: the real chain runs ATF at EL3 and U-Boot/Linux at
     * EL2. With a firmware substitute (QEMU PSCI) the payload starts at
     * EL2 under TCG; hardware accelerators provide EL1 only.
     */
    object_initialize_child(OBJECT(machine), "soc", &s->soc, TYPE_FSL_IMX8MM);
    object_property_set_str(soc, "cpu-type", machine->cpu_type, &error_abort);
    object_property_set_uint(soc, "num-cpus", machine->smp.cpus, &error_abort);
    object_property_set_bool(soc, "has-el3", mode == KICK_BOOT_ROM,
                             &error_abort);
    object_property_set_bool(soc, "has-el2", !accel_hw, &error_abort);
    /* The SOM's AR8033 replaces the MAC's built-in PHY */
    object_property_set_bool(soc, "enet-phy-connected", false, &error_abort);
    object_property_set_uint(OBJECT(&s->soc.ocotp), "unique-id", s->soc_uid,
                             &error_abort);
    /* Without ATF nothing would enable the system counter */
    object_property_set_bool(OBJECT(&s->soc.sysctr), "start-enabled",
                             mode != KICK_BOOT_ROM, &error_abort);
    kick_board_setup_sai2(s);
    /* Scan-out size the host sees until the guest programs a mode */
    qdev_prop_set_uint32(DEVICE(&s->soc.lcdif), "panel-width", s->panel_width);
    qdev_prop_set_uint32(DEVICE(&s->soc.lcdif), "panel-height",
                         s->panel_height);
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    memory_region_add_subregion(get_system_memory(), FSL_IMX8MM_RAM_START,
                                machine->ram);

    /* Fuse the MAC address the NIC uses, as on a programmed SOM */
    imx8mm_ocotp_set_mac(&s->soc.ocotp, &s->soc.enet.conf.macaddr);

    kick_board_wire_phy(s);
    kick_board_wire_pmic(s);
    kick_board_wire_expander(s);
    kick_board_wire_bridge(s);
    kick_board_wire_gt911(s);
    kick_board_wire_pcf85263(s);
    kick_board_wire_cv(s);

    kick_board_attach_card(s, 3, s->emmc, TYPE_EMMC);
    kick_board_attach_card(s, 2, s->sd, TYPE_SD_CARD);

    s->pwrkey_release = timer_new_ms(QEMU_CLOCK_VIRTUAL, kick_pwrkey_release,
                                     s);
    s->powerdown.notify = kick_powerdown_req;
    qemu_register_powerdown_notifier(&s->powerdown);

    if (qtest_enabled()) {
        return;
    }

    s->boot_info = (struct arm_boot_info) {
        .loader_start = FSL_IMX8MM_RAM_START,
        .board_id = -1,
        .ram_size = machine->ram_size,
        .psci_conduit = QEMU_PSCI_CONDUIT_SMC,
        .modify_dtb = kick_modify_dtb,
    };

    switch (mode) {
    case KICK_BOOT_KERNEL:
        if (!machine->kernel_filename) {
            /*
             * No payload from the machine: something else (-device loader
             * with cpu-num=0, a debugger) provides the code. QEMU still acts
             * as the PSCI firmware.
             */
            info_report("kick: no -kernel, CPU 0 starts where the guest "
                        "image loader points it");
            s->boot_info.firmware_loaded = true;
        }
        break;
    case KICK_BOOT_UBOOT:
        if (!machine->firmware) {
            /* U-Boot placed by -device loader,file=...,cpu-num=0 */
            s->boot_info.firmware_loaded = true;
            break;
        }
        if (load_image_targphys(machine->firmware, s->uboot_addr,
                                machine->ram_size - (s->uboot_addr -
                                                     FSL_IMX8MM_RAM_START),
                                &error_fatal) < 0) {
            exit(1);
        }
        /* U-Boot proper starts on CPU 0 at its link address */
        object_property_set_uint(OBJECT(&s->soc.cpu[0]), "rvbar",
                                 s->uboot_addr, &error_abort);
        s->boot_info.firmware_loaded = true;
        break;
    case KICK_BOOT_ROM: {
        bool sd = !strcmp(s->rom_boot_device, "sd");
        const char *drive = sd ? s->sd : s->emmc;
        BlockBackend *blk = drive ? blk_by_name(drive) : NULL;

        if (!blk) {
            error_report("kick: boot-mode=rom needs the %s drive",
                         sd ? "sd" : "emmc");
            exit(1);
        }
        /* SD on uSDHC2, eMMC on uSDHC3 (0-based instance numbers) */
        kick_bootrom_setup(&s->soc, blk,
                           sd ? KICK_BOOTDEV_SD : KICK_BOOTDEV_MMC,
                           sd ? 1 : 2, &error_fatal);
        /* ATF provides PSCI; QEMU must not intercept SMC */
        s->boot_info.psci_conduit = QEMU_PSCI_CONDUIT_DISABLED;
        s->boot_info.firmware_loaded = true;
        break;
    }
    }

    arm_load_kernel(&s->soc.cpu[0], machine, &s->boot_info);
}

/*
 * Board-level state after every system reset: the GPIO models clear their
 * pad levels when they reset, so pulled-up lines are driven again after
 * all devices have been reset.
 */
static void kick_machine_reset(MachineState *machine, ResetType type)
{
    qemu_devices_reset(type);
    kick_board_i2c_pullups(KICK_MACHINE(machine));
}

static const char *kick_get_default_cpu_type(const MachineState *ms)
{
    if (hvf_enabled() || kvm_enabled()) {
        return ARM_CPU_TYPE_NAME("host");
    }
    return ARM_CPU_TYPE_NAME("cortex-a53");
}

#define KICK_STR_PROP(field)                                      \
    static char *kick_get_##field(Object *obj, Error **errp)                \
    {                                                                       \
        return g_strdup(KICK_MACHINE(obj)->field);                          \
    }                                                                       \
    static void kick_set_##field(Object *obj, const char *v, Error **errp)  \
    {                                                                       \
        KickMachineState *s = KICK_MACHINE(obj);                            \
        g_free(s->field);                                                   \
        s->field = g_strdup(v);                                             \
    }

KICK_STR_PROP(boot)
KICK_STR_PROP(emmc)
KICK_STR_PROP(sd)
KICK_STR_PROP(cv_chardev)
KICK_STR_PROP(rom_boot_device)

#define KICK_INT_PROP(field, type)                                          \
    static void kick_get_##field(Object *obj, Visitor *v, const char *name, \
                                 void *opaque, Error **errp)                \
    {                                                                       \
        visit_type_##type(v, name, &KICK_MACHINE(obj)->field, errp);        \
    }                                                                       \
    static void kick_set_##field(Object *obj, Visitor *v, const char *name, \
                                 void *opaque, Error **errp)                \
    {                                                                       \
        visit_type_##type(v, name, &KICK_MACHINE(obj)->field, errp);        \
    }

KICK_INT_PROP(soc_uid, uint64)
KICK_INT_PROP(uboot_addr, uint64)
KICK_INT_PROP(emmc_boot_part_size, uint64)
KICK_INT_PROP(pwrkey_press_ms, uint32)
KICK_INT_PROP(panel_width, uint32)
KICK_INT_PROP(panel_height, uint32)

static bool kick_get_gt911_reset_wired(Object *obj, Error **errp)
{
    return KICK_MACHINE(obj)->gt911_reset_wired;
}

static void kick_set_gt911_reset_wired(Object *obj, bool v, Error **errp)
{
    KICK_MACHINE(obj)->gt911_reset_wired = v;
}

static void kick_instance_init(Object *obj)
{
    KickMachineState *s = KICK_MACHINE(obj);

    s->boot = g_strdup("kernel");
    s->rom_boot_device = g_strdup("emmc");
    s->uboot_addr = KICK_UBOOT_DEFAULT_ADDR;
    s->pwrkey_press_ms = KICK_PWRKEY_DEFAULT_PRESS_MS;
    s->emmc_boot_part_size = 0;
    s->soc_uid = 0;
    s->panel_width = KICK_PANEL_DEFAULT_WIDTH;
    s->panel_height = KICK_PANEL_DEFAULT_HEIGHT;
}

static void kick_finalize(Object *obj)
{
    KickMachineState *s = KICK_MACHINE(obj);

    g_free(s->boot);
    g_free(s->emmc);
    g_free(s->sd);
    g_free(s->cv_chardev);
    g_free(s->rom_boot_device);
}

static void kick_class_init(ObjectClass *oc, const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "Behringer KICK (VAR-SOM-MX8M-MINI, i.MX 8M Mini)";
    mc->init = kick_init;
    mc->reset = kick_machine_reset;
    mc->max_cpus = FSL_IMX8MM_NUM_CPUS;
    mc->default_cpus = FSL_IMX8MM_NUM_CPUS;
    mc->min_cpus = 1;
    mc->default_ram_size = 2 * GiB;
    mc->default_ram_id = "kick.ram";
    mc->get_default_cpu_type = kick_get_default_cpu_type;
    mc->no_cdrom = true;
    mc->no_floppy = true;
    mc->no_parallel = true;

    object_class_property_add_str(oc, "boot-mode", kick_get_boot,
                                  kick_set_boot);
    object_class_property_set_description(oc, "boot-mode",
        "Boot mode: kernel (-kernel/-dtb), uboot (-bios), rom (BootROM)");
    object_class_property_add_str(oc, "emmc", kick_get_emmc, kick_set_emmc);
    object_class_property_set_description(oc, "emmc",
        "Drive id of the eMMC image on uSDHC3");
    object_class_property_add_str(oc, "sd", kick_get_sd, kick_set_sd);
    object_class_property_set_description(oc, "sd",
        "Drive id of the SD card image on uSDHC2");
    object_class_property_add_str(oc, "cv-chardev", kick_get_cv_chardev,
                                  kick_set_cv_chardev);
    object_class_property_set_description(oc, "cv-chardev",
        "Chardev id for the CV/gate converter host protocol");
    object_class_property_add(oc, "soc-uid", "uint64", kick_get_soc_uid,
                              kick_set_soc_uid, NULL, NULL);
    object_class_property_set_description(oc, "soc-uid",
        "64-bit unique ID in the OCOTP fuses (soc0/serial_number)");
    object_class_property_add(oc, "uboot-addr", "uint64",
                              kick_get_uboot_addr, kick_set_uboot_addr,
                              NULL, NULL);
    object_class_property_set_description(oc, "uboot-addr",
        "boot-mode=uboot: load and entry address of U-Boot proper");
    object_class_property_add(oc, "emmc-boot-partition-size", "uint64",
                              kick_get_emmc_boot_part_size,
                              kick_set_emmc_boot_part_size, NULL, NULL);
    object_class_property_set_description(oc, "emmc-boot-partition-size",
        "Size of each eMMC boot partition, stored at the image start");
    object_class_property_add(oc, "power-key-press-ms", "uint32",
                              kick_get_pwrkey_press_ms,
                              kick_set_pwrkey_press_ms, NULL, NULL);
    object_class_property_set_description(oc, "power-key-press-ms",
        "How long system_powerdown holds the ON/OFF button");
    object_class_property_add(oc, "panel-width", "uint32",
                              kick_get_panel_width, kick_set_panel_width,
                              NULL, NULL);
    object_class_property_set_description(oc, "panel-width",
        "LCDIF scan-out width before the guest sets a mode");
    object_class_property_add(oc, "panel-height", "uint32",
                              kick_get_panel_height, kick_set_panel_height,
                              NULL, NULL);
    object_class_property_set_description(oc, "panel-height",
        "LCDIF scan-out height before the guest sets a mode");
    object_class_property_add_bool(oc, "gt911-reset-wired",
                                   kick_get_gt911_reset_wired,
                                   kick_set_gt911_reset_wired);
    object_class_property_set_description(oc, "gt911-reset-wired",
        "Connect GPIO5_IO03 to the GT911 RSTB input");
    /* "audiodev" (the generic machine property) feeds SAI2 */
    machine_add_audiodev_property(mc);
    object_class_property_add_str(oc, "rom-boot-device",
                                  kick_get_rom_boot_device,
                                  kick_set_rom_boot_device);
    object_class_property_set_description(oc, "rom-boot-device",
        "boot-mode=rom: boot device the fuses select, emmc or sd");
}

static const TypeInfo kick_machine_types[] = {
    {
        .name          = TYPE_KICK_MACHINE,
        .parent        = TYPE_MACHINE,
        .instance_size = sizeof(KickMachineState),
        .instance_init = kick_instance_init,
        .instance_finalize = kick_finalize,
        .class_init    = kick_class_init,
        .interfaces    = aarch64_machine_interfaces,
    },
};

DEFINE_TYPES(kick_machine_types)
