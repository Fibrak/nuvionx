#!/bin/bash
# Build NuvioNX inside MSYS2 with devkitPro installed at /opt/devkitpro.
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=/opt/devkitpro/devkitA64
export PATH=/opt/devkitpro/tools/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/portlibs/switch/bin:$PATH
cd "$(dirname "$0")"
make -j"$(nproc)" "$@"
