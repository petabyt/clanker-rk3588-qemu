#!/bin/bash
# Stock Armbian disk boot, own firmware, no injected boot inputs.
set -eu
cd "$(dirname "$0")/.."

make qemu
python3 scripts/test-rk-boot-devices.py
python3 scripts/run-opi5-raw.py --qemu build/qemu/qemu-system-aarch64 \
    --fresh --timeout "${P20_TIMEOUT:-900}"
echo "P20 OK: stock disk autoboot, serial login, 8 Linux CPUs, no serial input, unchanged boot region, clean diagnostics"
