#!/usr/bin/env python3
"""
Build images/emmc.img: a 64 MiB raw image with a DOS/MBR partition
table whose single partition (type 0x83, LBA 2048) holds an ext4
filesystem populated from a small root directory.  Used as the
-drive if=sd unit-1 (eMMC) image for the P11 storage milestone:
the kernel scans mmcblk0 partitions and the demo mounts
/dev/mmcblk0p1 as ext4 and verifies /hello.txt.

Host tools: mke2fs (e2fsprogs) with the -E offset= and -d directory
options; the MBR is written by this script (no fdisk dependency).
"""

import os
import subprocess
import sys

IMG = "images/emmc.img"
SIZE = 64 * 1024 * 1024
PART_LBA = 2048
FS_BLOCKS = 60 * 1024  # 60 MiB at 1 KiB blocks

root = "/tmp/opencode/emmc-root"
os.makedirs(root, exist_ok=True)
with open(os.path.join(root, "hello.txt"), "w") as f:
    f.write("eMMC rootfs test 0123456789\n")

if os.path.exists(IMG):
    os.unlink(IMG)
with open(IMG, "wb") as f:
    f.truncate(SIZE)

subprocess.run(
    [
        "mke2fs", "-q", "-t", "ext4", "-L", "emmc-test",
        "-b", "1024", "-E", f"offset={PART_LBA * 512}",
        "-d", root, IMG, str(FS_BLOCKS),
    ],
    check=True,
)

with open(IMG, "r+b") as f:
    # MBR: entry 1 at 0x1be = (boot, chs x3, type, chs x3, lba, sectors)
    entry = bytearray(16)
    entry[0] = 0x00            # not bootable
    entry[1:4] = b"\x00" * 3   # CHS start (ignored: LBA used)
    entry[4] = 0x83            # Linux filesystem
    entry[5:8] = b"\x00" * 3   # CHS end
    lba = PART_LBA.to_bytes(4, "little")
    sectors = (FS_BLOCKS * 1024 // 512).to_bytes(4, "little")
    entry[8:12] = lba
    entry[12:16] = sectors
    f.seek(0x1be)
    f.write(bytes(entry))
    f.seek(510)
    f.write(b"\x55\xaa")

print(f"{IMG}: {os.path.getsize(IMG)} bytes, ext4 at LBA {PART_LBA}")
