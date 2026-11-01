/*
 * Behringer KICK board: wiring constants
 *
 * Addresses come from imx8mm-var-som.dtsi and the KICK device tree patch
 * (Linux/buildroot/board/freescale/imx8mm_var_som/patches/linux).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef KICK_BOARD_H
#define KICK_BOARD_H

/* SOM devices */
#define KICK_PMIC_I2C_ADDR          0x4b    /* BD71847 on I2C1 */
#define KICK_ENET_PHY_ADDR          4       /* AR8033 on the ENET1 MDIO bus */

/*
 * Carrier devices written as separate models. The board instantiates
 * them when a model with this QOM type name is compiled in.
 */
#define KICK_TYPE_GT911             "goodix-gt911"
#define KICK_GT911_I2C_ADDR         0x14    /* I2C3, INT GPIO5_IO04, RST IO03 */
#define KICK_TYPE_PCF85263          "pcf85263"
#define KICK_PCF85263_I2C_ADDR      0x68    /* I2C3 */
#define KICK_TYPE_CV                "ad5675r"
#define KICK_CV_I2C_ADDR            0x0c    /* I2C4, /dev/i2c-3 in Linux */

/* Defaults of machine properties */
#define KICK_UBOOT_DEFAULT_ADDR     0x40200000  /* CONFIG_SYS_TEXT_BASE */
#define KICK_PWRKEY_DEFAULT_PRESS_MS 200

#endif /* KICK_BOARD_H */
