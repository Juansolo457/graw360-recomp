# The Steam Deck build environment (#38): Ubuntu 22.04, so the programs need glibc 2.35, below
# every SteamOS 3 release's, with the toolchain and libraries ReXGlue's own linux-amd64 releases
# are built with (its .github/workflows/_build-platform.yaml): clang 20 from apt.llvm.org, CMake
# from Kitware (jammy's 3.22 is below the 3.25 minimum) and libstdc++ 13 from the toolchain PPA
# (jammy's 11 has no <format>). tools/build-deck.sh builds and runs it with podman.
FROM docker.io/library/ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl wget gnupg lsb-release software-properties-common git python3 \
        binutils file \
    && wget -qO /tmp/llvm.sh https://apt.llvm.org/llvm.sh && chmod +x /tmp/llvm.sh \
    && /tmp/llvm.sh 20 && rm /tmp/llvm.sh \
    && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
    && wget -qO- https://apt.kitware.com/keys/kitware-archive-latest.asc \
        | gpg --dearmor -o /usr/share/keyrings/kitware.gpg \
    && echo "deb [signed-by=/usr/share/keyrings/kitware.gpg] https://apt.kitware.com/ubuntu/ jammy main" \
        > /etc/apt/sources.list.d/kitware.list \
    && apt-get update && apt-get install -y --no-install-recommends \
        clang-20 lld-20 cmake ninja-build build-essential g++-13 autoconf python3-venv unzip zip \
        libgtk-3-dev libx11-xcb-dev libxss-dev libvulkan-dev \
        libwayland-dev libwayland-bin wayland-protocols libxkbcommon-dev libdecor-0-dev \
        libasound2-dev libpulse-dev libpipewire-0.3-dev \
    && update-alternatives --install /usr/bin/clang clang /usr/bin/clang-20 200 \
    && update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-20 200 \
    && rm -rf /var/lib/apt/lists/*
