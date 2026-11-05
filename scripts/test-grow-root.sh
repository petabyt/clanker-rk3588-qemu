#!/bin/bash
# Host-side mechanical test for the fhs-grow-root flow baked into
# fhs-core/os/scripts/build-raw-rk3588.sh (§12 of FHS.md).
# Extracts the GROW heredoc (single source of truth), then: exact 7 GiB
# image geometry -> btrfs + seed -> grow backing file to 30 GiB (simulates
# dd onto a bigger card) -> run -> assert table/kernel/fs/GPT postconditions
# + both idempotence paths. Needs sudo (SUDO_ASKPASS=... or passwordless).
# Usage: SUDO_ASKPASS=... bash scripts/test-grow-root.sh
set -euo pipefail
SCRATCH="${SCRATCH:-$(mktemp -d /tmp/opencode/grow-fulltest.XXXX)}"
BUILD="${BUILD:-/mnt/fhs-core/os/scripts/build-raw-rk3588.sh}"
cd "$SCRATCH"
[ -f "$BUILD" ] || { echo "FAIL: build script not found: $BUILD"; exit 1; }
export SUDO_ASKPASS="${SUDO_ASKPASS:-/tmp/opencode/askpass.sh}"

echo "=== [0] extract fhs-grow-root from the build heredoc"
python3 - "$BUILD" "$SCRATCH/grow-test.sh" <<'EOF0'
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r"<<'GROW'\n(.*?)\nGROW\n", src, re.S)
assert m, "GROW heredoc not found"
body = m.group(1)
assert "LIMIT" not in body and "lsblk -bnbo" not in body, "stale refs in grow script"
open(sys.argv[2], "w").write(body + "\n")
EOF0
bash -n "$SCRATCH/grow-test.sh"

echo "=== [1] create 7G fixture with exact image geometry + assert numbering"
truncate -s 7G growfix.img
sudo -A sfdisk growfix.img >/dev/null <<'EOF2'
label: gpt
unit: sectors
first-lba: 2048

start=1081344, size=12582912, type=B921B045-1DF0-41C3-AF44-4C6F280D3FAE, name="rootfs"
start=32768, size=1048576, type=0FC63DAF-8483-4772-8E79-3D69D8477DE4, name="boot", attrs=LegacyBIOSBootable
EOF2
LOOP=$(sudo -A losetup -fP --show growfix.img)
M="$SCRATCH/mnt"; mkdir -p "$M"
cleanup() { sudo -A umount "$M" 2>/dev/null || true; sudo -A losetup -d "$LOOP" 2>/dev/null || true; }
trap cleanup EXIT
P1S=$(lsblk -no START "${LOOP}p1"); P2S=$(lsblk -no START "${LOOP}p2")
[ "$P1S" = "1081344" ] && [ "$P2S" = "32768" ] || { echo "FAIL numbering: p1=$P1S p2=$P2S"; exit 1; }
echo "PASS: numbering by declaration order (rootfs=p1@1081344, boot=p2@32768)"
sudo -A mkfs.btrfs -f "${LOOP}p1" >/dev/null
sudo -A mount "${LOOP}p1" "$M"
sudo -A dd if=/dev/zero of="$M/seed" bs=1M count=100 status=none

echo "=== [2] simulate flash onto a 30G medium: truncate + loop capacity reread"
truncate -s 30G growfix.img
sudo -A losetup -c "$LOOP"
DISKSECT=$(sudo -A blockdev --getsz "$LOOP")
[ "$DISKSECT" = "62914560" ] || { echo "FAIL: disk size $DISKSECT != 62914560"; exit 1; }
echo "PASS: device now 62914560 sectors (table still old — as after a raw dd)"

echo "=== [3] run the extracted fhs-grow-root"
sudo -A env FHS_ROOTPART="${LOOP}p1" FHS_MARKER="$SCRATCH/grown.marker" \
  bash "$SCRATCH/grow-test.sh"

echo "=== [4] assertions"
NEWSIZE=$((62914560 - 34 - 1081344))   # 61833182
DUMP=$(sudo -A sfdisk --dump "$LOOP")
echo "$DUMP" | grep -q "last-lba: 62914526" || { echo "FAIL: last-lba not relocated"; echo "$DUMP"; exit 1; }
echo "PASS: last-lba relocated to 62914526 (backup GPT at true end of 30G)"
echo "$DUMP" | grep 'name="rootfs"' | grep -q "start=     1081344, size=    ${NEWSIZE}" \
  || { echo "FAIL: p1 table entry wrong"; echo "$DUMP"; exit 1; }
echo "PASS: p1 grown in table to ${NEWSIZE} sectors (29.5G)"
echo "$DUMP" | grep 'name="boot"' | grep -q 'start=       32768, size=     1048576' \
  || { echo "FAIL: p2 moved/resized"; exit 1; }
echo "$DUMP" | grep 'name="boot"' | grep -q 'LegacyBIOSBootable' \
  || { echo "FAIL: p2 bootable attribute lost"; exit 1; }
echo "PASS: p2 untouched (32768/512M) + LegacyBIOSBootable preserved"
KSIZE=$(sudo -A blockdev --getsz "${LOOP}p1")
[ "$KSIZE" = "$NEWSIZE" ] || { echo "FAIL: kernel view $KSIZE sectors != $NEWSIZE"; exit 1; }
echo "PASS: kernel sees grown p1 (${KSIZE} sectors = $((KSIZE * 512)) bytes)"
sudo -A python3 - "$LOOP" <<'EOF3'
import sys
with open(sys.argv[1], "rb") as f:
    f.seek(62914559 * 512)
    sig = f.read(8)
assert sig == b"EFI PART", f"backup GPT signature at true end: {sig!r}"
print("PASS: backup GPT header ('EFI PART') at the true last LBA of 30G")
EOF3
FSSIZE=$(df -BM --output=size "$M" | tail -1 | tr -dc '0-9')
[ "$FSSIZE" -gt 30000 ] || { echo "FAIL: btrfs only ${FSSIZE}M"; exit 1; }
echo "PASS: btrfs resized online -> ${FSSIZE}M (~29.5G)"
[ -f "$SCRATCH/grown.marker" ] || { echo "FAIL: marker missing"; exit 1; }
echo "PASS: one-shot marker written"

echo "=== [5] idempotence"
set +e
OUT=$(sudo -A env FHS_ROOTPART="${LOOP}p1" FHS_MARKER="$SCRATCH/grown.marker" \
  bash "$SCRATCH/grow-test.sh" 2>&1); RC=$?
set -e
[ "$RC" -eq 0 ] || { echo "FAIL: marker-path rerun rc=$RC: $OUT"; exit 1; }
echo "PASS: marker present -> silent no-op, rc 0"
rm -f "$SCRATCH/grown.marker"
set +e
OUT=$(sudo -A env FHS_ROOTPART="${LOOP}p1" FHS_MARKER="$SCRATCH/grown.marker" \
  bash "$SCRATCH/grow-test.sh" 2>&1); RC=$?
set -e
[ "$RC" -eq 0 ] && echo "$OUT" | grep -q "already grown" \
  || { echo "FAIL: grow-path rerun rc=$RC out: $OUT"; exit 1; }
[ -f "$SCRATCH/grown.marker" ] || { echo "FAIL: marker not recreated"; exit 1; }
echo "PASS: marker lost -> 'already grown' no-op, rc 0, marker recreated"

echo
echo "ALL GROW MECHANICAL TESTS PASSED"
