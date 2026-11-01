# KICK (BE 0700-AAM) SoC, board and device models.
#
# Peripheral models go into system_ss, the SoC and board into arm_common_ss.
# Keep one line per model and keep the list sorted to make merges trivial.
system_ss.add(when: 'CONFIG_IMX8MM_DISPMIX', if_true: files('imx8mm_dispmix.c'))
system_ss.add(when: 'CONFIG_IMX8MM_LCDIF', if_true: files('imx8mm_lcdif.c'))
system_ss.add(when: 'CONFIG_IMX8MM_MIPI_DSIM', if_true: files('imx8mm_mipi_dsim.c'))
system_ss.add(when: 'CONFIG_KICK_DISPLAY_BENCH', if_true: files('kick-display-bench.c'))
system_ss.add(when: 'CONFIG_SN65DSI83', if_true: files('sn65dsi83.c'))
system_ss.add(when: 'CONFIG_VIVANTE_GPU', if_true: files('vivante_de.c', 'vivante_gpu.c'))
