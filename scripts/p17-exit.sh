#!/bin/bash
# P17 exit: boot Linux on the modeled eDP0 chain (mainline analogix_dp),
# assert probe + link-train serial evidence, the QEMU-side AUX/DPCD trace,
# a non-black 1920x1080 screendump carrying the demo colour bands, and a
# clean -d unimp,guest_errors,invalid_mem log.  The initramfs init idles on
# a shell (no guest poweroff), so QEMU is ended via monitor `quit` and both
# return codes are asserted.
set -u
cd "$(dirname "$0")/.."

mkdir -p /tmp/opencode
rm -f /tmp/p17s.log /tmp/p17mon.sock /tmp/p17.ppm qemu-p17.log \
      /tmp/opencode/p17-trace.log

make -s linux-dtb >/dev/null
make -s images/rk3588-qemu-edp.dtb 2>/dev/null || make images/rk3588-qemu-edp.dtb

timeout 150 build/qemu/qemu-system-aarch64 -M rk3588 -display none \
    -serial file:/tmp/p17s.log \
    -monitor unix:/tmp/p17mon.sock,server,nowait \
    -bios experiments/atf/build/rk3588/release/bl31/bl31.elf \
    -device loader,file=experiments/u-boot/u-boot.bin,addr=0x800000,force-raw=on \
    -device loader,file=linux/linux-7.2.9/arch/arm64/boot/Image,addr=0x02000000,force-raw=on \
    -device loader,file=images/initramfs.bin,addr=0x09000000,force-raw=on \
    -device loader,file=images/rk3588-qemu-edp.dtb,addr=0x0a100000,force-raw=on \
    -dtb experiments/u-boot/dts/dt.dtb \
    -drive if=sd,file=images/emmc.img,format=raw,unit=1 \
    -trace enable=rk3588_edp_*,file=/tmp/opencode/p17-trace.log \
    -d unimp,guest_errors,invalid_mem -D qemu-p17.log &
QPID=$!

# Connect to the monitor up front (no socket race), wait for the demo to
# finish (LINUX-OK is its last marker), screendump the eDP-scanned-out
# console, then quit — the guest itself never powers off here.
python3 - <<'EOF'
import socket, time, sys

s = None
for _ in range(240):                       # monitor connect retry <= 120s
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(5)
        s.connect("/tmp/p17mon.sock")
        break
    except OSError:
        s = None
        time.sleep(0.5)
if s is None:
    print("monitor connect failed")
    sys.exit(1)

trigger = b"LINUX-OK"                      # demo's final marker
for _ in range(2800):                      # poll <= 140s
    try:
        data = open("/tmp/p17s.log", "rb").read()
    except FileNotFoundError:
        data = b""
    if trigger in data:
        break
    time.sleep(0.05)
else:
    print("LINUX-OK never appeared")
    sys.exit(1)

time.sleep(0.3)                            # let the last fbcon draw land
s.sendall(b"screendump /tmp/p17.ppm hdmi\n")
time.sleep(0.5)
s.sendall(b"quit\n")
time.sleep(0.5)
EOF
MON_RC=$?

wait $QPID
QRC=$?

echo "qemu rc=$QRC (0 = monitor quit), monitor rc=$MON_RC"
echo "--- serial tail ---"
tr -d '\r' < /tmp/p17s.log | tail -n 30
echo "--- AUX/DPCD trace ---"
cat /tmp/opencode/p17-trace.log 2>/dev/null | head -40
echo "--- checks ---"
python3 - "$QRC" "$MON_RC" <<'EOF'
import sys, collections, re
qrc, mrc = int(sys.argv[1]), int(sys.argv[2])
fails = []

serial = open("/tmp/p17s.log", "rb").read().decode("utf-8", "replace")
serial = serial.replace("\r", "")

# --- serial: probe, EDID-derived native mode, fbcon takeover, demo
for pat, why in [
    ("bound fdec0000.edp", "analogix_dp component bound"),
    ("Console: switching to colour frame buffer device 240x67",
     "fbcon native 1920x1080 grid (240x67) from the eDP EDID"),
    ("[PASS] mmap /dev/fb0", "demo wrote the colour bands"),
    ("LINUX-OK", "demo completed"),
]:
    if pat not in serial:
        fails.append(f"serial missing: {pat!r} ({why})")

# --- serial: link training / probe failures must be absent
for pat in ["unable to do link train", "failed to set bridge",
            "AUX CH enable timeout", "failed to get hpd",
            "EQ Max loop", "CR Max reached", "dp commit error",
            "failed to get rockchip,grf", "no DP phy configured",
            "failed to get clock", "no chip-data", "[FAIL]"]:
    if pat in serial:
        fails.append(f"serial contains failure marker: {pat!r}")

# --- QEMU-side AUX trace: EDID read + the DPCD training handshake
try:
    trace = open("/tmp/opencode/p17-trace.log").read()
except FileNotFoundError:
    trace = ""
if not trace:
    fails.append("AUX trace log empty/missing")
else:
    if not re.search(r"AUX i2c read addr=0x050", trace):
        fails.append("trace missing EDID I2C read at slave 0x50")
    for pat in ["DPCD[0x102] <- 0x21",     # training pattern 1 (CR)
                "DPCD[0x102] <- 0x22",     # training pattern 2 (EQ)
                "AUX dpcd read addr=0x202" # link-status poll
                ]:
        if pat not in trace:
            fails.append(f"trace missing: {pat!r}")

# --- clean QEMU log
try:
    qlog = open("qemu-p17.log").read()
except FileNotFoundError:
    qlog = "MISSING"
if qlog.strip():
    fails.append(f"qemu-p17.log not empty ({len(qlog)} bytes)")

# --- pixel proof: 1920x1080, four demo bands intact, not black
try:
    data = open("/tmp/p17.ppm", "rb").read()
    parts = data.split(b"\n", 3)
    assert parts[0] == b"P6", f"not a P6 ppm: {parts[0]!r}"
    w, h = map(int, parts[1].split())
    pixels = parts[3]
    assert len(pixels) == w * h * 3, (len(pixels), w * h * 3)
    cnt = collections.Counter(pixels[i:i + 3] for i in range(0, len(pixels), 3))
    print(f"screendump {w}x{h}, distinct colors: {len(cnt)}")
    for c, n in cnt.most_common(6):
        print(f"  #{c.hex()} x{n}")
    if (w, h) != (1920, 1080):
        fails.append(f"screendump {w}x{h}, expected 1920x1080")
    for name, c in [("green", b"\x00\xff\x00"), ("blue", b"\x00\x00\xff"),
                    ("white", b"\xff\xff\xff"), ("red", b"\xff\x00\x00")]:
        n = cnt.get(c, 0)
        if n < 500000:
            fails.append(f"band {name} #{c.hex()} only {n} px (<500000)")
except FileNotFoundError:
    fails.append("no screendump /tmp/p17.ppm")

if qrc != 0 or mrc != 0:
    fails.append(f"return codes: qemu rc={qrc} monitor rc={mrc}")

if fails:
    print("P17 FAIL:")
    for f in fails:
        print("  -", f)
    sys.exit(1)
print("P17 OK: analogix_dp probed on eDP0, AUX/EDID + link training "
      "exercised, fbcon at native 1920x1080, bands rendered, logs clean")
EOF
RC=$?
echo "P17_SCRIPT_RC=$RC"
exit $RC
