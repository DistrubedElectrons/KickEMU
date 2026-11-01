# dtdoc stage: generates the device tree node table (Markdown) for the board DTB, the
# source of the generated part of Emulator/docs/board-dt.md. Not part of "all".
#
# shellcheck shell=bash

stage_dtdoc() {
    local dts dtb header
    dts="$(br BR2_LINUX_KERNEL_INTREE_DTS_NAME | awk '{print $1}')"
    dtb="${LINUX_OBJ}/arch/arm64/boot/dts/${dts}.dtb"
    header="${LINUX_SRC}/include/dt-bindings/clock/imx8mm-clock.h"
    [[ -f "${dtb}" ]] || die "dtdoc: build the kernel stage first"
    python3 "${HERE}/../tools/dt-summary.py" "${dtb}" "${header}" "$(basename "${dtb}")" \
        > "${WORK}/build/board-dt-table.md"
    publish "${WORK}/build/board-dt-table.md"
    dtc -q -I dtb -O dts -o "${WORK}/build/$(basename "${dts}").dts" "${dtb}"
    publish "${WORK}/build/$(basename "${dts}").dts"
}
