#!/usr/bin/env python3
"""Patch Pixel mosey_server for the Galaxy S10e (Exynos 9820, ARMv8.2, BCM4375).

1. Radio backend: mosey_server (Pixel 10, moseyservice v2025_09_23) calls
       mosey_start_4(..., radio = 2)      ; 2 = wonder0 on phy "wonder" (nl80211 vendor cmds)
   at file offset 0x929a4 (`mov w4, #2`). radio = 1 selects the "radiotap0" backend
   (existing monitor iface + ART_* private commands on wlan0), which moseynex.so emulates.

2. ARMv8.3 instructions the S10e CPU lacks (no paca / lrcpc in /proc/cpuinfo), in executable
   sections only:
     retaa              -> ret      (paciasp/autiasp are hint-space NOPs here, so x30 is unsigned)
     ldapr{b,h,,x} Rt   -> ldar{b,h,,x} Rt   (LDAR is strictly stronger ordering: safe)

usage: patch_mosey_server.py <in> <out>
"""
import hashlib
import struct
import sys

OFF = 0x929A4
ORIG = bytes.fromhex("44008052")     # mov w4, #2
PATCH = bytes.fromhex("24008052")    # mov w4, #1

RETAA, RETAB, RET = 0xD65F0BFF, 0xD65F0FFF, 0xD65F03C0
LDAPR_MASK, LDAPR_BITS = 0x3FFFFC00, 0x38BFC000   # any size: bits[31:30]
LDAR_BITS = 0x08DFFC00


def exec_sections(data):
    shoff = struct.unpack_from("<Q", data, 0x28)[0]
    shentsize, shnum = struct.unpack_from("<HH", data, 0x3A)
    for i in range(shnum):
        _, _, flags, _, off, size = struct.unpack_from("<IIQQQQ", data, shoff + i * shentsize)
        if flags & 0x4:   # SHF_EXECINSTR
            yield off, size


def patch_armv82(data):
    n_ret = n_ldapr = 0
    for off, size in exec_sections(data):
        for i in range(off, off + size - 3, 4):
            w = struct.unpack_from("<I", data, i)[0]
            if w in (RETAA, RETAB):
                struct.pack_into("<I", data, i, RET)
                n_ret += 1
            elif (w & LDAPR_MASK) == LDAPR_BITS:
                struct.pack_into("<I", data, i, (w & 0xC0000000) | LDAR_BITS | (w & 0x3FF))
                n_ldapr += 1
    return n_ret, n_ldapr


def main():
    src, dst = sys.argv[1], sys.argv[2]
    data = bytearray(open(src, "rb").read())
    if data[:4] != b"\x7fELF":
        sys.exit("not an ELF file")
    cur = bytes(data[OFF:OFF + 4])
    if cur == PATCH:
        print("radio: already patched")
    elif cur != ORIG:
        sys.exit(f"unexpected bytes at {OFF:#x}: {cur.hex()} (different mosey_server build?)")
    data[OFF:OFF + 4] = PATCH
    n_ret, n_ldapr = patch_armv82(data)
    open(dst, "wb").write(data)
    print(f"radio {OFF:#x}: {ORIG.hex()} -> {PATCH.hex()}; retaa->ret x{n_ret}; ldapr->ldar x{n_ldapr}; "
          f"sha256={hashlib.sha256(data).hexdigest()[:16]}")


if __name__ == "__main__":
    main()
