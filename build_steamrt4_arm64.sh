#!/bin/bash

base_dir=$(pwd)/build_steamrt4_arm64
dist_dir=$base_dir/dist

meson setup \
    --wrap-mode=default \
    -D android-libbacktrace=disabled \
    -D android-libperfetto=disabled \
    -D gallium-va=disabled \
    -D gallium-mediafoundation=disabled \
    -D gallium-drivers= \
    -D gbm=disabled \
    -D glvnd=disabled \
    -D libunwind=disabled \
    -D microsoft-clc=disabled \
    -D vulkan-drivers=freedreno \
    -D llvm=disabled \
    -D lmsensors=disabled \
    -D perfetto=true \
    -D shader-cache=enabled \
    -Dc_args="-march=armv8.2-a" \
    -Dcpp_args="-march=armv8.2-a" \
    --buildtype=release \
    -D b_ndebug=true \
    --prefix=$dist_dir build

