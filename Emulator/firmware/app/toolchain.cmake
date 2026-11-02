# CMake toolchain file for KT_Kick: Buildroot's own toolchain file from the SDK
# ($BR_SDK/share/buildroot/toolchainfile.cmake) with three changes JUCE needs.
#
# An environment variable carries the SDK path because CMake re-reads toolchain files in
# try_compile projects, which do not see the main project's cache.

include("$ENV{BR_SDK}/share/buildroot/toolchainfile.cmake")

# 1. Buildroot names the system "Buildroot" (Platform/Buildroot.cmake includes the Linux
#    platform), but JUCE and Kick test CMAKE_SYSTEM_NAME STREQUAL "Linux" for the Linux
#    sources, pkg-config dependencies and -latomic.
set(CMAKE_SYSTEM_NAME Linux)

# 2. The toolchain file exports PKG_CONFIG_SYSROOT_DIR into the environment. JUCE builds its
#    helper tool juceaide for the build host in a nested CMake run that inherits the
#    environment, so host pkg-config would return target paths there. The SDK's pkg-config
#    wrapper (found through CMAKE_PROGRAM_PATH) sets the sysroot itself.
unset(ENV{PKG_CONFIG_SYSROOT_DIR})

# 3. JUCE compiles with -flto (juce_recommended_lto_flags). Static libraries of LTO objects
#    need the plugin-aware archiver.
# CMAKE_C_COMPILER is <host>/bin/<tuple>-gcc; gcc-ar and gcc-ranlib sit next to it.
set(_br_prefix "${CMAKE_C_COMPILER}")
if(EXISTS "${_br_prefix}-ar")
    set(CMAKE_AR "${_br_prefix}-ar" CACHE FILEPATH "" FORCE)
    set(CMAKE_RANLIB "${_br_prefix}-ranlib" CACHE FILEPATH "" FORCE)
    set(CMAKE_C_COMPILER_AR "${_br_prefix}-ar" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER_AR "${_br_prefix}-ar" CACHE FILEPATH "" FORCE)
    set(CMAKE_C_COMPILER_RANLIB "${_br_prefix}-ranlib" CACHE FILEPATH "" FORCE)
    set(CMAKE_CXX_COMPILER_RANLIB "${_br_prefix}-ranlib" CACHE FILEPATH "" FORCE)
endif()
