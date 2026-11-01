# Bare-metal AArch64 toolchain: clang with --target=aarch64-none-elf and an ELF lld.
#
# The compiler and linker are searched in this order, each can be forced from the
# command line (-DKICK_CLANG=... -DKICK_LLD=...):
#   clang:  Homebrew LLVM, then the clang on PATH
#   lld:    ld.lld (Homebrew lld/LLVM or PATH), then rust-lld from a Rust toolchain
#           (called with -flavor gnu)

set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

find_program(KICK_CLANG NAMES clang
             HINTS /opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin)
find_program(KICK_LLD NAMES ld.lld
             HINTS /opt/homebrew/opt/lld/bin /opt/homebrew/opt/llvm/bin
                   /usr/local/opt/llvm/bin)
if(NOT KICK_LLD)
    file(GLOB _rust_lld "$ENV{HOME}/.rustup/toolchains/*/lib/rustlib/*/bin/rust-lld")
    if(_rust_lld)
        list(GET _rust_lld 0 _first)
        set(KICK_LLD ${_first} CACHE FILEPATH "ELF linker" FORCE)
    endif()
endif()
# rust-lld is the generic lld driver and needs to be told it links ELF
set(KICK_LLD_FLAVOR "")
if(KICK_LLD MATCHES "rust-lld$")
    set(KICK_LLD_FLAVOR "-flavor gnu")
endif()
if(NOT KICK_CLANG OR NOT KICK_LLD)
    message(FATAL_ERROR "need clang and ld.lld (brew install llvm lld) or rust-lld")
endif()

set(CMAKE_C_COMPILER ${KICK_CLANG})
set(CMAKE_ASM_COMPILER ${KICK_CLANG})
set(CMAKE_C_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_C_LINK_EXECUTABLE
    "${KICK_LLD} ${KICK_LLD_FLAVOR} <LINK_FLAGS> <OBJECTS> -o <TARGET> <LINK_LIBRARIES>")
