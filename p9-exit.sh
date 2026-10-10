#!/bin/bash
# P9 exit: boot the demo, screendump the VOP2 console after the payload's
# final screen draws, then let the guest's PSCI_SYSTEM_OFF shut QEMU down
# (rc must be 0 — this also verifies the shutdown model, see plan.md P8).
set -u
cd "$(dirname "$0")"

rm -f /tmp/p9s.log /tmp/p9mon.sock /tmp/p9.ppm qemu-p9.log

timeout 90 build/qemu/qemu-system-aarch64 -M rk3588 -display none \
    -serial file:/tmp/p9s.log \
    -monitor unix:/tmp/p9mon.sock,server,nowait \
    -bios rk/genbook_demo.img \
    -d unimp,guest_errors,invalid_mem -D qemu-p9.log &
QPID=$!

# Connect to the monitor up front (no socket race), wait for the payload's
# memory-map block (early enough: the 4 Range lines + nop loop + poweroff
# still leave ~0.5s), screendump — and do NOT quit: the guest powers itself
# off (PSCI_SYSTEM_OFF), which must end QEMU with rc 0.
python3 - <<'EOF'
import socket, time, sys

s = None
for _ in range(120):                       # monitor connect retry <= 60s
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect("/tmp/p9mon.sock")
        break
    except OSError:
        s = None
        time.sleep(0.5)
if s is None:
    print("monitor connect failed")
    sys.exit(1)

trigger = b"Memory description map:"           # last draws follow it;
for _ in range(1600):                       # dump before poweroff; poll <= 80s
    try:
        data = open("/tmp/p9s.log", "rb").read()
    except FileNotFoundError:
        data = b""
    if trigger in data:
        break
    time.sleep(0.05)
else:
    print("trigger line never appeared")
    sys.exit(1)

time.sleep(0.05)
s.sendall(b"screendump /tmp/p9.ppm\n")
time.sleep(0.3)
EOF
MON_RC=$?

wait $QPID
QRC=$?

echo "qemu rc=$QRC (0 = guest PSCI shutdown), monitor rc=$MON_RC"
echo "--- serial log ---"
cat /tmp/p9s.log
echo "--- screendump check ---"
python3 - "$QRC" "$MON_RC" <<'EOF'
import sys, collections
qrc, mrc = int(sys.argv[1]), int(sys.argv[2])
if qrc != 0 or mrc != 0:
    print(f"P9 FAIL: qemu rc={qrc} monitor rc={mrc} (guest must self-shutdown with rc 0)")
    sys.exit(1)
data = open("/tmp/p9.ppm", "rb").read()
parts = data.split(b"\n", 3)
assert parts[0] == b"P6", f"not a P6 ppm: {parts[0]!r}"
w, h = map(int, parts[1].split())
pixels = parts[3]
assert len(pixels) == w * h * 3, (len(pixels), w * h * 3)
cnt = collections.Counter(pixels[i:i + 3] for i in range(0, len(pixels), 3))
print(f"screendump {w}x{h}, distinct colors: {len(cnt)}")
for c, n in cnt.most_common(5):
    print(f"  #{c.hex()} x{n}")
bg = cnt.get(bytes([0x43, 0x98, 0xd7]), 0)   # demo BACKGROUND_COLOR
white = cnt.get(bytes([0xff, 0xff, 0xff]), 0)  # font colour
print(f"bg(4398d7) px={bg}, white px={white}")
if len(cnt) < 2 or bg == 0 or white == 0:
    print("P9 FAIL: expected rendered framebuffer content in screendump")
    sys.exit(1)
print("P9 OK: VOP2 scanout rendered; guest PSCI_SYSTEM_OFF exited QEMU (rc=0)")
EOF
RC=$?
echo "P9_SCRIPT_RC=$RC"
exit $RC
