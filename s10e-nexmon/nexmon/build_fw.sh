#!/bin/bash
# Build the nexmon-patched BCM4375B1 18.41.117 STA firmware (seemoo-lab/nexmon PR #663)
# in WSL/Linux x86_64.  Output: $OUT/bcmdhd_nex.bin_b1
#
#   bash build_fw.sh [nexmon_dir] [out_dir]
#
# Needs: make gcc g++ bison flex gawk qpdf(zlib-flate) xxd libtool automake autoconf texinfo glibc.i686
set -e
HERE=$(dirname "$(readlink -f "$0")")
RATE_PATCH=$HERE/inject-rate.patch
NEX=${1:-$HOME/nexmon}
OUT=${2:-$HERE/out}
PATCH=patches/bcm4375b1/18_41_117_sta/nexmon

if [ ! -d "$NEX" ]; then
    git clone --depth 1 https://github.com/seemoo-lab/nexmon.git "$NEX"
fi
cd "$NEX"
if [ ! -d "$PATCH" ]; then
    git fetch --depth 1 origin pull/663/head:pr663
    git checkout pr663 -- patches/bcm4375b1/18_41_117_sta
fi
# mosey: per-frame radiotap rate (default 54 Mbit/s instead of 6) on the VO FIFO (idempotent)
if git apply --check -R "$RATE_PATCH" 2>/dev/null; then
    echo "inject-rate patch already applied"
else
    git apply "$RATE_PATCH"
fi
# mosey: netdev data-path injection (dngl_sendwl hook + hnd_pktfetch), diag ioctl 0x614
DP_PATCH=$HERE/datapath.patch
if git apply --check -R "$DP_PATCH" 2>/dev/null; then
    echo "datapath patch already applied"
else
    git apply "$DP_PATCH"
fi

source setup_env.sh
# buildtools (gcc plugin, b43 asm, ucode/flashpatch extractors) + per-firmware extraction
# only buildtools: the full root `make` also downloads every firmware (bcm4373a0 download fails)
make buildtools >"$NEX/build-root.log" 2>&1 || { tail -40 "$NEX/build-root.log"; exit 1; }

cd "$PATCH"
make clean >/dev/null 2>&1 || true
make 2>&1 | tail -40
ls -la *.bin_b1 gen/ 2>/dev/null | head

mkdir -p "$OUT"
cp -v bcmdhd_sta.bin_b1 "$OUT/bcmdhd_nex.bin_b1"
sha256sum "$OUT/bcmdhd_nex.bin_b1"
