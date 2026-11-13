# syntax=docker/dockerfile:1
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
FROM debian:trixie-slim

# Install minimal toolchain: ARM GCC 14 (C++23), Newlib, libstdc++, Make, Python3, HIDAPI and
# zopfli (the dashboard's gzip stream, tools/embed_web.py).
# g++, clang and libclang-rt-dev are the HOST compilers of the sanitizer gate step (tools/static_check.sh sanitize:
# every host test under ASan + UBSan with gcc and with clang); the firmware itself is built by the ARM toolchain only.
# llvm supplies llvm-symbolizer, so a clang sanitizer report names the function and line instead of a bare offset.
RUN apt-get update && apt-get install -y --no-install-recommends \
    gcc-arm-none-eabi \
    binutils-arm-none-eabi \
    libnewlib-arm-none-eabi \
    libstdc++-arm-none-eabi-newlib \
    g++ \
    clang \
    libclang-rt-dev \
    llvm \
    make \
    python3 \
    python3-pip \
    zopfli \
    libhidapi-hidraw0 \
    udev \
    && rm -rf /var/lib/apt/lists/* \
    && pip3 install --no-cache-dir --break-system-packages hidapi

WORKDIR /workspace

CMD ["make"]
