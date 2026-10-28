#!/usr/bin/env python3
"""Regenerate the xex-boot golden fixtures from dir2atr (needs dir2atr and
repos/MyPicoDOS/picobootcode-dd.bin). Not run by the tests.

  generate.py <picobootcode-dd.bin> <outdir>

Each case is an executable of a given size (content from synth(), mirrored in
tests/test_xex_boot_image.cpp), staged as autorun.xex and turned into an ATR
with the exact command boot-disk.mk uses (`dir2atr -m -d`; -D is identical for
programs that fit 720 sectors and fails for larger ones). Output per case:
<name>.txt with the sector count, the FNV-1a 64 hash of the ATR payload
(everything after the 16-byte header) and hex dumps of selected sectors.
"""
import os, shutil, subprocess, sys, tempfile

CASES = [("small", 1012), ("n720", 707 * 253), ("n727", 707 * 253 + 1),
         ("n1199", 300059), ("n3575", 900000), ("n4367", 1100000)]
SELECT = [1, 2, 3, 4, 359, 360, 361, 369]


def synth(size):
    b = bytearray(size)
    for i in range(size):
        b[i] = (i * 131 + (i >> 8) * 7 + 13) & 0xFF
    b[0] = b[1] = 0xFF
    return bytes(b)


def fnv(data):
    h = 0xCBF29CE484222325
    for c in data:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def sector(body, n):
    if n <= 3:
        return body[(n - 1) * 128:n * 128]
    return body[384 + (n - 4) * 256:384 + (n - 3) * 256]


def main():
    boot, out = sys.argv[1], sys.argv[2]
    for name, size in CASES:
        with tempfile.TemporaryDirectory() as td:
            stage = os.path.join(td, "s")
            os.mkdir(stage)
            data = synth(size)
            open(os.path.join(stage, "autorun.xex"), "wb").write(data)
            atr = os.path.join(td, "o.atr")
            subprocess.run(["dir2atr", "-m", "-d", "-B", boot, atr, stage], check=True, capture_output=True)
            body = open(atr, "rb").read()[16:]
            n = (len(body) - 384) // 256 + 3
            lines = ["size %d" % size, "sectors %d" % n, "fnv %016x" % fnv(body)]
            for s in SELECT + [n]:
                if s <= n:
                    lines.append("sector %d %s" % (s, sector(body, s).hex()))
            open(os.path.join(out, name + ".txt"), "w").write("\n".join(lines) + "\n")
            if name == "small":
                open(os.path.join(out, "small.xex"), "wb").write(data)


main()
