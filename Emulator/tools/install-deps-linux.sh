#!/usr/bin/env bash
#
# Build dependencies of KickEmu on Debian 12+/Ubuntu 24.04+ (x86_64 or arm64):
# QEMU with the kick machine, the KickEmu host (JUCE) and the bare-metal demo.
#
# Usage: sudo Emulator/tools/install-deps-linux.sh

set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
apt-get update
apt-get install -y --no-install-recommends \
    build-essential git python3 python3-venv python3-tomli ninja-build meson pkg-config cmake \
    flex bison libglib2.0-dev libpixman-1-dev libslirp-dev libfdt-dev zlib1g-dev \
    libpipewire-0.3-dev libpulse-dev libasound2-dev libusb-1.0-0-dev \
    libfreetype-dev libfontconfig1-dev libx11-dev libxext-dev libxrandr-dev libxinerama-dev libxcursor-dev \
    clang lld \
    ca-certificates socat
