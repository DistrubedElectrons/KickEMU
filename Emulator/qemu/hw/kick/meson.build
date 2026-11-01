# KICK (BE 0700-AAM) SoC, board and device models.
#
# Peripheral models go into system_ss, the SoC and board into arm_common_ss.
# Keep one line per model and keep the list sorted to make merges trivial.
system_ss.add(when: 'CONFIG_IMX8MM_DISPMIX', if_true: files('imx8mm_dispmix.c'))
system_ss.add(when: 'CONFIG_IMX8MM_LCDIF', if_true: files('imx8mm_lcdif.c'))
system_ss.add(when: 'CONFIG_KICK_DISPLAY_BENCH', if_true: files('kick-display-bench.c'))
