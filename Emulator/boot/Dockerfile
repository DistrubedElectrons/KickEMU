# Builder for the KICK boot chain and kernel.
#
# Debian bullseye ships GCC 10.2.1, the same GCC release as the Buildroot 2021.02 default
# external toolchain used by KTKick_defconfig (Arm GNU-A 10.2-2020.11). That toolchain is
# only published for x86_64 hosts, so on arm64 the native bullseye GCC 10.2 is used
# instead of running an x86 cross compiler under emulation.
ARG BASE_IMAGE=debian:bullseye
FROM ${BASE_IMAGE}

ARG DEBIAN_FRONTEND=noninteractive
# bullseye has left deb.debian.org and lives on archive.debian.org. The base image
# already carries packages from the security archive, so both archives are configured.
ARG APT_MIRROR=http://archive.debian.org/debian
ARG APT_SECURITY_MIRROR=http://archive.debian.org/debian-security
RUN . /etc/os-release \
    && rm -f /etc/apt/sources.list.d/* \
    && printf 'deb %s %s main\ndeb %s %s-security main\n' \
        "${APT_MIRROR}" "${VERSION_CODENAME}" \
        "${APT_SECURITY_MIRROR}" "${VERSION_CODENAME}" > /etc/apt/sources.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        bc bison build-essential ca-certificates cpio curl device-tree-compiler \
        dosfstools e2fsprogs fdisk file flex gcc-10 g++-10 gdisk git jq kmod \
        libconfuse-dev libelf-dev libssl-dev libgnutls28-dev mtools patch pkg-config \
        python3 python3-dev python3-setuptools rsync swig xz-utils \
        zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

# genimage is not packaged for bullseye. The version and download location are taken
# from Buildroot's package/genimage/genimage.mk by the build script and passed in here.
ARG GENIMAGE_VERSION
ARG GENIMAGE_URL
RUN test -n "${GENIMAGE_VERSION}" && test -n "${GENIMAGE_URL}" \
    && mkdir -p /tmp/genimage && cd /tmp/genimage \
    && curl -fsSL "${GENIMAGE_URL}" | tar -xJ --strip-components=1 \
    && ./configure --prefix=/usr/local >/dev/null \
    && make -j"$(nproc)" >/dev/null && make install >/dev/null \
    && cd / && rm -rf /tmp/genimage \
    && genimage --version

RUN gcc --version | head -n 1
