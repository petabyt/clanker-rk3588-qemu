#!/bin/sh
# Build the P5 initramfs: busybox-static + static demo program + /init
# -> images/initramfs.bin (cpio.gz padded to a fixed 4 MiB so the
# u-boot booti initrd size operand can be a constant 0x400000).

set -e
cd "$(dirname "$0")"

ROOT=rootfs
rm -rf "$ROOT"
mkdir -p "$ROOT/bin" "$ROOT/sbin" "$ROOT/etc" "$ROOT/proc" "$ROOT/sys" \
         "$ROOT/dev" "$ROOT/tmp" "$ROOT/mnt" "$ROOT/root"
# Device nodes via gen_init_cpio "nod" entries: mknod is not permitted
# inside this container, and the kernel opens /dev/console (c 5:1) before
# running /init — without it init runs with no stdio and exits silently.

DEB=/tmp/opencode/busybox-static_arm64.deb
[ -f "$DEB" ] || curl -s -o "$DEB" \
    https://deb.debian.org/debian/pool/main/b/busybox/busybox-static_1.37.0-6+b9_arm64.deb
rm -rf /tmp/opencode/busybox-dpkg
dpkg -x "$DEB" /tmp/opencode/busybox-dpkg
cp /tmp/opencode/busybox-dpkg/usr/bin/busybox "$ROOT/bin/busybox"

aarch64-linux-gnu-gcc -static -O2 -Wall -o "$ROOT/bin/demo" demo.c

cp init "$ROOT/init"
chmod +x "$ROOT/init" "$ROOT/bin/busybox" "$ROOT/bin/demo"

# cpio list for gen_init_cpio: walk the tree, then add the device nodes
# the kernel needs before devtmpfs is mounted.
LIST=initramfs.cpio.list
{
	( cd "$ROOT" && find . -mindepth 1 | sed -e 's|^\./||' ) | sort |
	while read -r p; do
		if [ -d "$ROOT/$p" ]; then
			echo "dir /$p 0755 0 0"
		elif [ -L "$ROOT/$p" ]; then
			echo "slink /$p $(readlink "$ROOT/$p") 0777 0 0"
		else
			mode=$(stat -c %a "$ROOT/$p")
			echo "file /$p $ROOT/$p 0$mode 0 0"
		fi
	done
	echo "nod /dev/console 0600 0 0 c 5 1"
	echo "nod /dev/null 0666 0 0 c 1 3"
} > "$LIST"
/tmp/opencode/gen_init_cpio "$LIST" | gzip -9 > initramfs.cpio.gz
size=$(stat -c %s initramfs.cpio.gz)
echo "initramfs.cpio.gz: $size bytes"
[ "$size" -le 4194304 ] || { echo "too big for the 4 MiB slot" >&2; exit 1; }
cp initramfs.cpio.gz ../images/initramfs.bin
truncate -s 4194304 ../images/initramfs.bin
echo "images/initramfs.bin: $(stat -c %s ../images/initramfs.bin) bytes (padded)"
