# KICK bare-metal demo firmware for the i.MX8M Mini.
#
#   cmake -S Emulator/demo/baremetal -B Emulator/out/demo \
#         -DCMAKE_TOOLCHAIN_FILE=Emulator/demo/baremetal/cmake/aarch64-none-elf.cmake
#   cmake --build Emulator/out/demo
#
# Produces kick-demo.elf (load with -kernel) and kick-demo.bin.

cmake_minimum_required(VERSION 3.22)

if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE ${CMAKE_CURRENT_SOURCE_DIR}/cmake/aarch64-none-elf.cmake)
endif()

project(kick_demo C ASM)

# Version shown in the banner: branch and commit of the checkout
execute_process(COMMAND git rev-parse --abbrev-ref HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE KICK_GIT_BRANCH OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
execute_process(COMMAND git rev-parse --short HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
                OUTPUT_VARIABLE KICK_GIT_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
set(KICK_DEMO_VERSION "1.0.0")

# Board configuration, all overridable with -D
set(KICK_LOAD_ADDR   0x40100000 CACHE STRING "Link and load address in DDR")
set(KICK_FB_ADDR     0x60000000 CACHE STRING "Framebuffer address in DDR")
set(KICK_FB_WIDTH    1024       CACHE STRING "Framebuffer width (pixels)")
set(KICK_FB_HEIGHT   600        CACHE STRING "Framebuffer height (pixels)")
set(KICK_TICK_HZ     100        CACHE STRING "GPT1 tick rate")
set(KICK_MCU_BAUD    230400     CACHE STRING "UART1 baud rate to the MCU")
set(KICK_CONSOLE_BAUD 115200    CACHE STRING "UART4 console baud rate")

add_executable(kick-demo.elf
    src/start.S
    src/main.c
    src/console.c
    src/fmt.c
    src/uart.c
    src/gic.c
    src/gpt.c
    src/clock.c
    src/soc_info.c
    src/i2c.c
    src/gpio.c
    src/mmu.c
    src/psci.c
    src/lcdif.c
    src/gfx.c
    src/pktmgr.c
    src/panel.c
    src/screen.c
)

target_compile_definitions(kick-demo.elf PRIVATE
    KICK_GIT_BRANCH="${KICK_GIT_BRANCH}"
    KICK_GIT_COMMIT="${KICK_GIT_COMMIT}"
    KICK_DEMO_VERSION="${KICK_DEMO_VERSION}"
    KICK_FB_ADDR=${KICK_FB_ADDR}
    KICK_FB_WIDTH=${KICK_FB_WIDTH}
    KICK_FB_HEIGHT=${KICK_FB_HEIGHT}
    KICK_TICK_HZ=${KICK_TICK_HZ}
    KICK_MCU_BAUD=${KICK_MCU_BAUD}
    KICK_CONSOLE_BAUD=${KICK_CONSOLE_BAUD}
)

# Freestanding, no FP/SIMD (no FPU context to manage in the IRQ handler)
target_compile_options(kick-demo.elf PRIVATE
    -ffreestanding -fno-builtin -nostdlib -mgeneral-regs-only -mstrict-align
    -fno-stack-protector -fno-pic -O2 -g -Wall -Wextra -std=c11
    -mcpu=cortex-a53
)
target_include_directories(kick-demo.elf PRIVATE src)

set(KICK_LDSCRIPT ${CMAKE_CURRENT_SOURCE_DIR}/src/kick-demo.ld)
target_link_options(kick-demo.elf PRIVATE
    -T ${KICK_LDSCRIPT} --defsym=KICK_LOAD_ADDR=${KICK_LOAD_ADDR}
    -nostdlib -static --gc-sections)
set_target_properties(kick-demo.elf PROPERTIES LINK_DEPENDS ${KICK_LDSCRIPT})

find_program(KICK_OBJCOPY NAMES llvm-objcopy rust-objcopy
             HINTS /opt/homebrew/opt/llvm/bin)
if(KICK_OBJCOPY)
    add_custom_command(TARGET kick-demo.elf POST_BUILD
        COMMAND ${KICK_OBJCOPY} -O binary kick-demo.elf kick-demo.bin
        COMMENT "kick-demo.bin")
endif()
