# KICK (BE 0700-AAM) SoC, board and device models.
#
# Peripheral models go into system_ss, the SoC and board into arm_common_ss.
# Keep one line per model and keep the list sorted to make merges trivial.
system_ss.add(when: 'CONFIG_IMX8MM_DISPMIX', if_true: files('imx8mm_dispmix.c'))
system_ss.add(when: 'CONFIG_IMX8MM_LCDIF', if_true: files('imx8mm_lcdif.c'))
system_ss.add(when: 'CONFIG_IMX8MM_MIPI_DSIM', if_true: files('imx8mm_mipi_dsim.c'))
system_ss.add(when: 'CONFIG_KICK_AD5675R', if_true: files('ad5675r.c'))
system_ss.add(when: 'CONFIG_KICK_DISPLAY_BENCH', if_true: files('kick-display-bench.c'))
system_ss.add(when: 'CONFIG_KICK_GOODIX_GT911', if_true: files('goodix_gt911.c'))
system_ss.add(when: 'CONFIG_KICK_IMX8MM_SAI', if_true: files('imx8mm_sai.c'))
system_ss.add(when: 'CONFIG_KICK_IMX_SDMA', if_true: files('imx_sdma.c'))
system_ss.add(when: 'CONFIG_KICK_PCA9534', if_true: files('pca9534.c'))
system_ss.add(when: 'CONFIG_KICK_PCF85263', if_true: files('pcf85263.c'))
system_ss.add(when: 'CONFIG_SN65DSI83', if_true: files('sn65dsi83.c'))
system_ss.add(when: 'CONFIG_VIVANTE_GPU', if_true: files('vivante_de.c', 'vivante_gpu.c'))
arm_common_ss.add(when: 'CONFIG_KICK_PERIPH_TEST', if_true: files('kick-periph-test.c'))
