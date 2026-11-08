# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Yaraslau Furman
"""Mount the synthesized FAT16 image (dumped by tests/test_vfat) with an independent reader.

The reader below follows the Microsoft FAT spec only; it shares no code with src/drivers/vfat.cpp. When
fsck.fat is installed it must also pass `fsck.fat -n`. Skips when the image was not dumped
(run through `make test`, which sets K2_DUMP=host-build).
"""
import os
import shutil
import struct
import subprocess
import unittest

IMG = os.path.join(os.environ.get("K2_DUMP", "host-build"), "vfat.img")


class Fat16:
    def __init__(self, data):
        self.d = data
        (self.bps, self.spc, self.rsv, self.nfat, self.rootn, self.tot16) = struct.unpack_from("<HBHBHH", data, 11)
        self.fatsz = struct.unpack_from("<H", data, 22)[0]
        self.root_lba = self.rsv + self.nfat * self.fatsz
        self.data_lba = self.root_lba + (self.rootn * 32 + self.bps - 1) // self.bps
        self.nclusters = (self.tot16 - self.data_lba) // self.spc
        assert 4085 <= self.nclusters < 65525, "not FAT16"

    def fat(self, c):
        return struct.unpack_from("<H", self.d, self.rsv * self.bps + 2 * c)[0]

    def chain(self, c):
        out = []
        while 2 <= c < 0xFFF8:
            out.append(c)
            c = self.fat(c)
        return out

    def cluster(self, c):
        o = (self.data_lba + (c - 2) * self.spc) * self.bps
        return self.d[o:o + self.spc * self.bps]

    def read_chain(self, c):
        return b"".join(self.cluster(x) for x in self.chain(c))

    def entries(self, raw):
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F:
                continue
            name, attr = e[:11], e[11]
            cl, size = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0]
            yield name.decode("latin1"), attr, cl, size

    def walk(self, raw=None, prefix=""):
        if raw is None:
            o = self.root_lba * self.bps
            raw = self.d[o:o + self.rootn * 32]
        for name, attr, cl, size in self.entries(raw):
            if attr & 0x08 or name.startswith("."):
                continue
            n = name[:8].strip() + ("." + name[8:].strip() if name[8:].strip() else "")
            if attr & 0x10:
                yield prefix + n + "/", None
                yield from self.walk(self.read_chain(cl), prefix + n + "/")
            else:
                yield prefix + n, self.read_chain(cl)[:size] if cl else b""


@unittest.skipUnless(os.path.exists(IMG), "no host-build/vfat.img (run make test)")
class VfatImage(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(IMG, "rb") as f:
            cls.data = f.read()
        cls.fs = Fat16(cls.data)

    def test_tree(self):
        files = dict(self.fs.walk())
        self.assertEqual(sorted(files), ["BIG.DAT", "EMARKERS/", "EMARKERS/A.BIN", "EMPTY.TXT", "README.TXT"])

    def test_contents_follow_the_test_pattern(self):
        files = dict(self.fs.walk())
        self.assertEqual(files["README.TXT"], bytes((31 + i) & 0xFF for i in range(100)))
        self.assertEqual(files["EMARKERS/A.BIN"], bytes((62 + i) & 0xFF for i in range(66)))
        self.assertEqual(files["BIG.DAT"], bytes((124 + i) & 0xFF for i in range(5000)))
        self.assertEqual(files["EMPTY.TXT"], b"")

    def test_volume_label(self):
        o = self.fs.root_lba * 512
        self.assertEqual(self.data[o:o + 11], b"K2 DRIVE   ")
        self.assertEqual(self.data[o + 11], 0x08)

    def test_chains_are_contiguous_and_not_shared(self):
        seen = set()
        for name, attr, cl, size in self.fs.entries(self.data[self.fs.root_lba * 512:self.fs.data_lba * 512]):
            ch = self.fs.chain(cl)
            self.assertFalse(seen & set(ch), name)
            seen |= set(ch)
            self.assertEqual(ch, list(range(ch[0], ch[0] + len(ch))) if ch else [])

    @unittest.skipUnless(shutil.which("fsck.fat"), "fsck.fat not installed")
    def test_fsck_clean(self):
        r = subprocess.run(["fsck.fat", "-n", IMG], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


IMG_LFN = os.path.join(os.environ.get("K2_DUMP", "host-build"), "vfat_lfn.img")


def lfn_names(fs, raw):
    """Reassemble (long name, short name) pairs from raw directory bytes, checking the checksum."""
    out, parts, want = [], [], None
    for i in range(0, len(raw), 32):
        e = raw[i:i + 32]
        if e[0] == 0:
            break
        if e[11] == 0x0F:
            parts.append(e)
            continue
        if e[0] == 0x2E or e[11] & 0x08:
            continue
        if parts:
            s = 0
            for ch in e[:11]:
                s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
            assert parts[0][0] & 0x40
            for n, p in zip(range(len(parts), 0, -1), parts):
                assert p[0] & 0x3F == n and p[13] == s
            # pieces arrive last-first; rebuild in ascending order
            pieces = []
            for p in reversed(parts):
                chars = struct.unpack("<13H", p[1:11] + p[14:26] + p[28:32])
                for c in chars:
                    if c in (0, 0xFFFF):
                        break
                    pieces.append(chr(c))
            out.append(("".join(pieces), e[:11].decode()))
        parts = []
    return out


@unittest.skipUnless(os.path.exists(IMG_LFN), "no vfat_lfn.img (run make test)")
class VfatLongNames(unittest.TestCase):
    def test_names_and_fsck(self):
        with open(IMG_LFN, "rb") as f:
            data = f.read()
        fs = Fat16(data)
        o = fs.root_lba * 512
        root = data[o:o + fs.rootn * 32]
        ent = [e for e in fs.entries(root) if e[0].startswith("CABLES")][0]
        names = lfn_names(fs, fs.read_chain(ent[2]))
        self.assertEqual(len(names), 40)
        for i, (long_name, short) in enumerate(names):
            self.assertEqual(long_name, "Apple-USB4-240W-%02d.json" % i)
            self.assertEqual(short, "CAB%02d   JSN" % i)
        if shutil.which("fsck.fat"):
            r = subprocess.run(["fsck.fat", "-n", IMG_LFN], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)


if __name__ == "__main__":
    unittest.main()
