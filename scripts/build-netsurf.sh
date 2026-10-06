#!/bin/bash
# Build NetSurf for ARM (framebuffer target)
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Check if we have ARM cross-compile toolchain
CROSS_PREFIX=""
if [ -f "/opt/arm gcc/bin/arm-linux-gnueabihf-gcc" ]; then
    CROSS_PREFIX="/opt/arm gcc/bin/arm-linux-gnueabihf-"
elif command -v arm-linux-gnueabihf-gcc &>/dev/null; then
    CROSS_PREFIX="arm-linux-gnueabihf-"
elif [ -f "/mnt/SDCARD/System/bin/gcc" ]; then
    CROSS_PREFIX="/mnt/SDCARD/System/bin/arm-linux-gnueabihf-"
fi

# For native build on TrimUI
NATIVE_BUILD=true

if [ "$NATIVE_BUILD" = true ]; then
    echo "=== Building NetSurf for TrimUI (native) ==="

    # Install dependencies on Debian/Ubuntu
    if command -v apt-get &>/dev/null; then
        sudo apt-get install -y \
            build-essential \
            libcurl4-openssl-dev \
            libssl-dev \
            libpng-dev \
            libfreetype6-dev \
            libfontconfig1-dev \
            libnsfb-dev \
            libjs0-dev \
            bison \
            flex \
            pkg-config
    fi

    cd netsurf-all-3.11

    # Create Makefile.config for framebuffer
    cat > Makefile.config << 'EOF'
# NetSurf Framebuffer configuration
TARGET = framebuffer

# Disable JavaScript for lighter build
NETSURF_USE_DUKTAPE := NO

# Disable JPEG XL (heavy dependency)
NETSURF_USE_JPEGXL := NO

# Use internal font renderer (no freetype dependency)
NETSURF_FB_FONTLIB := internal

# Font paths for TrimUI
NETSURF_FB_FONTPATH := /mnt/SDCARD/Apps/RomCloud/fonts
NETSURF_FB_FONT_SANS_SERIF := DejaVuSans.ttf
NETSURF_FB_FONT_SANS_SERIF_BOLD := DejaVuSans-Bold.ttf
NETSURF_FB_FONT_MONOSPACE := DejaVuSansMono.ttf
NETSURF_FB_FONT_MONOSPACE_BOLD := DejaVuSansMono-Bold.ttf

# Resource path
NETSURF_FB_RESPATH := /mnt/SDCARD/Apps/RomCloud/netsurf/res
EOF

    # Build
    make clean || true
    make TARGET=framebuffer -j$(nproc)

    echo "=== Build complete: netsurf ==="
    ls -la netsurf

else
    echo "=== Cross-compiling NetSurf for ARM ==="

    if [ -z "$CROSS_PREFIX" ]; then
        echo "ERROR: No ARM cross-compile toolchain found"
        echo "Please install: apt-get install gcc-arm-linux-gnueabihf"
        exit 1
    fi

    export CC="${CROSS_PREFIX}gcc"
    export CXX="${CROSS_PREFIX}g++"
    export AR="${CROSS_PREFIX}ar"
    export RANLIB="${CROSS_PREFIX}ranlib"
    export STRIP="${CROSS_PREFIX}strip"

    cd netsurf-all-3.11

    # Create cross-compile config
    cat > Makefile.config << EOF
TARGET = framebuffer
HOST = ${CROSS_PREFIX%'-'}

NETSURF_USE_DUKTAPE := NO
NETSURF_USE_JPEGXL := NO
NETSURF_FB_FONTLIB := internal
EOF

    make TARGET=framebuffer HOST=${CROSS_PREFIX%'-'} -j$(nproc)
    ${CROSS_PREFIX}strip netsurf

    echo "=== Build complete ==="
    ls -la netsurf
fi
