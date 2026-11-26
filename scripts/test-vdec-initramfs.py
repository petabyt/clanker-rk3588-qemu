#!/usr/bin/env python3
"""Boot a RAM-root decoder discovery probe; this does not decode video."""

import argparse
import gzip
import importlib.util
import re
import zlib
import os
from pathlib import Path
import stat
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parent.parent

INIT = b'''#!/bin/busybox sh
/bin/busybox --install -s /bin 2>/dev/null
mount -t proc proc /proc
mount -t sysfs sysfs /sys
mount -t devtmpfs devtmpfs /dev
mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null
echo VDEC_PROBE_BEGIN
echo "kernel: $(uname -r)"
if [ -r /proc/config.gz ]; then
    zcat /proc/config.gz | grep '^CONFIG_VIDEO_ROCKCHIP_VDEC=' || :
fi
if grep -q 'vdec.disk=1' /proc/cmdline; then
    for i in $(seq 1 120); do
        [ -b /dev/mmcblk0p1 ] && break
        sleep 1
    done
    [ -b /dev/mmcblk0p1 ] || echo VDEC_DISK_NOT_FOUND
fi
if [ -b /dev/mmcblk0p1 ]; then
    mkdir -p /mnt
    if mount -t ext4 -o ro,noload /dev/mmcblk0p1 /mnt; then
        echo VDEC_DISK_MOUNTED_READONLY
        if [ -e /mnt/lib/modules/$(uname -r)/modules.dep ]; then
            mkdir -p /lib/modules
            if mount --bind /mnt/lib/modules /lib/modules; then
                if modprobe rockchip_vdec 2>&1; then
                    echo VDEC_MODPROBE_OK
                else
                    echo VDEC_MODPROBE_FAILED
                fi
            else
                echo VDEC_MODULE_BIND_FAILED
            fi
        else
            echo VDEC_DISK_MODULES_MISSING
        fi
    else
        echo VDEC_DISK_MOUNT_FAILED
    fi
elif [ -e /lib/modules/$(uname -r)/modules.dep ]; then
    if modprobe rockchip_vdec 2>&1; then
        echo VDEC_MODPROBE_OK
    else
        echo VDEC_MODPROBE_FAILED
    fi
elif ! grep -q 'vdec.disk=1' /proc/cmdline; then
    echo VDEC_MODULES_NOT_SUPPLIED
fi
echo VDEC_MODULE_STATE_BEGIN
grep -E '^(rockchip_vdec|videobuf2|v4l2|videodev|mc) ' /proc/modules || :
echo VDEC_PLATFORM_BEGIN
for d in /sys/bus/platform/devices/fdc38100* /sys/bus/platform/devices/fdc40100*; do
    [ -e "$d" ] || continue
    echo "device: $d"
    if [ -L "$d/driver" ]; then
        echo "driver: $(readlink -f "$d/driver")"
    else
        echo driver: UNBOUND
    fi
    [ ! -r "$d/modalias" ] || cat "$d/modalias"
done
echo VDEC_VIDEO_BEGIN
for d in /sys/class/video4linux/* /sys/class/media/*; do
    [ -e "$d" ] || continue
    echo "node: ${d##*/}"
    [ ! -r "$d/name" ] || cat "$d/name"
    echo "device: $(readlink -f "$d/device")"
    if grep -q 'vdec.ioctl=1' /proc/cmdline &&
       [ "$(readlink -f "$d/device")" = /sys/devices/platform/fdc38100.video-codec ]; then
        /bin/vdec-ioctl-probe "/dev/${d##*/}" || echo VDEC_IOCTL_FAILED
    fi
done
if grep -q 'vdec.request=1' /proc/cmdline; then
    /bin/vdec-request-test || echo VDEC_REQUEST_TEST_FAILED
fi
echo VDEC_DEFERRED_BEGIN
cat /sys/kernel/debug/devices_deferred 2>/dev/null
echo VDEC_DMESG_BEGIN
dmesg | grep -iE 'rkvdec|rockchip.vdec|video-codec|fdc38000|fdc40000' || :
echo VDEC_PROBE_DONE
exec /bin/sh
'''


def newc_entry(name, data, mode, ino):
    """Build a newc member without depending on the host cpio utility."""
    name = name.encode() + b'\0'
    fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, 0, 0, len(name), 0)
    header = b'070701' + b''.join(f'{v:08x}'.encode() for v in fields)
    result = header + name
    result += b'\0' * (-len(result) % 4)
    result += data
    return result + b'\0' * (-len(result) % 4)


def overlay(modules, ioctl_probe=None, request_probe=None):
    entries = [("init", INIT, stat.S_IFREG | 0o755)]
    if ioctl_probe is not None:
        entries.append(('bin/vdec-ioctl-probe', ioctl_probe, stat.S_IFREG | 0o755))
    if request_probe is not None:
        entries.append(('bin/vdec-request-test', request_probe, stat.S_IFREG | 0o755))
    if modules:
        # The caller supplies a complete lib/modules tree, including modules.dep.
        entries.extend([('lib', b'', stat.S_IFDIR | 0o755),
                        ('lib/modules', b'', stat.S_IFDIR | 0o755)])
        for path in sorted(modules.rglob('*')):
            relative = Path('lib/modules') / path.relative_to(modules)
            if path.is_symlink():
                if path.name in ('build', 'source'):
                    continue
                raise ValueError(f'unsupported module tree symlink: {path}')
            if path.is_dir():
                entries.append((str(relative), b'', stat.S_IFDIR | 0o755))
            elif path.is_file():
                entries.append((str(relative), path.read_bytes(), stat.S_IFREG | 0o644))
    entries.append(('TRAILER!!!', b'', 0))
    return gzip.compress(b''.join(newc_entry(*entry, i + 1)
                                  for i, entry in enumerate(entries)), mtime=0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--qemu', default=str(ROOT / 'build/qemu/qemu-system-aarch64'))
    parser.add_argument('--dtb', default=str(ROOT / 'images/armbian/rk3588s-orangepi-5-armbian.dtb'))
    parser.add_argument('--modules', type=Path, help='directory containing kernel-version module directories')
    parser.add_argument('--image', type=Path, help='Armbian SD image, attached through a disposable snapshot')
    parser.add_argument('--vdec0-power', action='store_true', help='enable experimental VDEC0 power transitions')
    parser.add_argument('--expect-vdec0', action='store_true', help='require VDEC0 to bind and expose a video node')
    parser.add_argument('--ioctl', action='store_true', help='open VDEC0 and enumerate V4L2 formats')
    parser.add_argument('--request', action='store_true', help='submit one H.264 request and expect controlled decoder error')
    parser.add_argument('--decode', action='store_true', help='submit a self-contained H.264 keyframe and verify NV12 output')
    parser.add_argument('--1080p', dest='fullhd', action='store_true', help='test one 1920x1080 keyframe')
    parser.add_argument('--frame', type=Path, help='self-contained single-IDR Annex B fixture')
    parser.add_argument('--reference', type=Path, help='matching tightly packed planar YUV420 reference')
    parser.add_argument('--p-frame', type=Path, help='submit a reference P slice after the IDR, expecting an error')
    parser.add_argument('--p-reference', type=Path, help='matching P-frame YUV420 reference; expect decode')
    parser.add_argument('--p2-frame', type=Path, help='second P slice, frame_num 2')
    parser.add_argument('--p2-reference', type=Path, help='second P-frame YUV420 reference')
    parser.add_argument('--sequence', type=Path, help='Annex B sequence for further P pictures')
    parser.add_argument('--sequence-nv12', type=Path, help='packed NV12 reference sequence')
    parser.add_argument('--corrupt-reference', action='store_true', help='corrupt IDR capture before P request; expect error')
    parser.add_argument('--cc', default='aarch64-linux-gnu-gcc', help='AArch64 cross compiler for --ioctl')
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    if bool(args.frame) != bool(args.reference):
        parser.error('--frame and --reference must be supplied together')
    if args.frame and not args.fullhd:
        parser.error('custom frames currently require --1080p')
    if args.p_frame and not args.frame:
        parser.error('--p-frame requires --frame and --reference')
    if args.p_reference and not args.p_frame:
        parser.error('--p-reference requires --p-frame')
    if args.corrupt_reference and (not args.p_frame or args.p_reference):
        parser.error('--corrupt-reference requires --p-frame without --p-reference')
    if bool(args.p2_frame) != bool(args.p2_reference) or (args.p2_frame and not args.p_reference):
        parser.error('--p2-frame and --p2-reference require --p-frame and --p-reference')
    if bool(args.sequence) != bool(args.sequence_nv12) or (args.sequence and not args.p2_frame):
        parser.error('--sequence and --sequence-nv12 require both P-picture fixtures')
    if args.fullhd:
        args.decode = True
    if args.decode:
        args.request = True
    if args.modules and not args.modules.is_dir():
        parser.error('--modules must point to a lib/modules directory')
    if args.image and not args.image.is_file():
        parser.error('--image must point to an existing disk image')

    spec = importlib.util.spec_from_file_location('desktop', ROOT / 'scripts/run-desktop.py')
    desktop = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(desktop)
    kernel = ROOT / 'images/armbian/vmlinuz-6.18.43-current-rockchip64'
    base = ROOT / 'images/initramfs.bin'
    bl31 = ROOT / 'experiments/atf/build/rk3588/release/bl31/bl31.elf'
    uboot = ROOT / 'experiments/u-boot/u-boot.bin'
    uboot_dtb = ROOT / 'experiments/u-boot/dts/dt.dtb'
    for path in (Path(args.qemu), Path(args.dtb), kernel, base, bl31, uboot, uboot_dtb):
        if not path.is_file():
            raise SystemExit(f'missing {path}')

    with tempfile.TemporaryDirectory(prefix='vdec-probe-', dir='/tmp/opencode') as tmp:
        initrd = Path(tmp) / 'initrd.img'
        probe = None
        if args.ioctl:
            binary = Path(tmp) / 'vdec-ioctl-probe'
            subprocess.run([args.cc, '-Os', '-Wall', '-Wextra', '-ffreestanding',
                            '-fno-builtin', '-fno-stack-protector', '-nostdlib',
                            '-static', '-Wl,-e,_start', '-o', str(binary),
                            str(ROOT / 'scripts/vdec-ioctl-probe.c')], check=True)
            probe = binary.read_bytes()
        request_probe = None
        if args.request:
            if args.frame:
                frame, reference = args.frame.resolve(), args.reference.resolve()
            else:
                flags = subprocess.check_output(['pkg-config', '--cflags', '--libs',
                                                 'libavcodec', 'libavutil'], text=True).split()
                generator = Path(tmp) / 'make-vector'
                subprocess.run(['cc', '-O2', str(ROOT / 'scripts/make-vdec-vector.c'),
                                '-o', str(generator), *flags], check=True)
                frame, reference = Path(tmp) / 'frame.h264', Path(tmp) / 'ref.yuv'
                subprocess.run([str(generator), str(frame), str(reference)] + (['1080p'] if args.fullhd else []), check=True)
            vector_spec = importlib.util.spec_from_file_location('vector', ROOT / 'scripts/inspect-vdec-vector.py')
            vector = importlib.util.module_from_spec(vector_spec)
            vector_spec.loader.exec_module(vector)
            nals = list(vector.annex_b(frame.read_bytes()))
            types = [n[0] & 31 for n in nals]
            if (types.count(7) != 1 or types.count(8) != 1 or types.count(5) != 1 or
                    any(t in (1, 2, 3, 4, 19, 20, 21) for t in types)):
                raise RuntimeError('test vector is not a single IDR')
            sps = vector.parse_sps(nals[types.index(7)])
            pps = vector.parse_pps(nals[types.index(8)], sps)
            pic = vector.parse_idr(nals[types.index(5)], sps, pps)
            dimensions = (1920, 1080) if args.fullhd else (64, 64)
            if reference.stat().st_size != dimensions[0] * dimensions[1] * 3 // 2:
                raise RuntimeError('reference size does not match dimensions')
            coded = ((sps['pic_width_in_mbs_minus1'] + 1) * 16 - 2 * sum(sps['crop'][:2]),
                     (sps['pic_height_in_map_units_minus1'] + 1) * 16 - 2 * sum(sps['crop'][2:]))
            if coded != dimensions:
                raise RuntimeError(f'unexpected vector dimensions: {coded}')
            sflags = (sps['frame_mbs_only_flag'] << 4 | sps['direct_8x8_inference_flag'] << 6 |
                      sps['gaps_in_frame_num_value_allowed'] << 3)
            pflags = (pps['entropy_coding_mode_flag'] | pps['bottom_field_pic_order_in_frame_present_flag'] << 1 |
                      pps['weighted_pred_flag'] << 2 | pps['deblocking_filter_control_present_flag'] << 3 |
                      pps['constrained_intra_pred_flag'] << 4 | pps['redundant_pic_cnt_present_flag'] << 5)
            vals = dict(V_SPS_PROFILE=sps['profile_idc'], V_SPS_CONSTRAINT=sps['constraint_set_flags'],
                        V_SPS_LEVEL=sps['level_idc'], V_SPS_ID=sps['seq_parameter_set_id'],
                        V_SPS_FRAME_BITS=sps['log2_max_frame_num_minus4'], V_SPS_POC_TYPE=sps['pic_order_cnt_type'],
                        V_SPS_POC_BITS=sps['log2_max_pic_order_cnt_lsb_minus4'], V_SPS_REFS=sps['max_num_ref_frames'],
                        V_SPS_WIDTH=sps['pic_width_in_mbs_minus1'], V_SPS_HEIGHT=sps['pic_height_in_map_units_minus1'],
                        V_SPS_FLAGS=sflags, V_PPS_ID=pps['pic_parameter_set_id'],
                        V_PPS_L0=pps['num_ref_idx_l0_default_active_minus1'], V_PPS_L1=pps['num_ref_idx_l1_default_active_minus1'],
                        V_PPS_QP=pps['pic_init_qp_minus26'], V_PPS_QS=pps['pic_init_qs_minus26'],
                        V_PPS_CHROMA=pps['chroma_qp_index_offset'], V_PPS_BIPRED=pps['weighted_bipred_idc'],
                        V_PPS_FLAGS=pflags, V_PIC_REF=pic['nal_ref_idc'], V_PIC_FRAME=pic['frame_num'],
                        V_PIC_IDR=pic['idr_pic_id'], V_PIC_POC=pic['pic_order_cnt_lsb'])
            idr = frame.read_bytes() if args.decode else b'\0\0\0\1' + nals[types.index(5)]
            vals['V_EXPECT_DECODE'] = int(args.decode)
            vals['V_WIDTH'], vals['V_HEIGHT'] = dimensions
            p_slice = b''
            if args.p_frame:
                pnals = list(vector.annex_b(args.p_frame.read_bytes()))
                slices = [n for n in pnals if (n[0] & 31) in (1, 2, 3, 4, 5, 19, 20, 21)]
                if len(slices) != 1 or slices[0][0] & 31 != 1:
                    raise RuntimeError('P fixture must contain exactly one non-IDR slice')
                b = vector.Bits(vector.rbsp(slices[0][1:]))
                mb, kind, pid = b.ue(), b.ue(), b.ue()
                frame_num = b.get(sps['log2_max_frame_num_minus4'] + 4)
                if (mb != 0 or kind % 5 != 0 or pid != pps['pic_parameter_set_id'] or
                        frame_num != 1 or sps['pic_order_cnt_type'] != 2 or
                        sps['max_num_ref_frames'] != 1 or not slices[0][0] & 0x60):
                    raise RuntimeError('unsupported P-frame fixture')
                p_slice = b'\0\0\0\1' + slices[0]
                vals['V_P_REF'] = (slices[0][0] >> 5) & 3
            vals.setdefault('V_P_REF', 0)
            vals['V_HAS_P'] = int(bool(p_slice))
            p2_slice = b''
            if args.p2_frame:
                p2nals = list(vector.annex_b(args.p2_frame.read_bytes()))
                p2s = [n for n in p2nals if (n[0] & 31) in (1, 2, 3, 4, 5, 19, 20, 21)]
                if len(p2s) != 1 or p2s[0][0] & 31 != 1 or not p2s[0][0] & 0x60:
                    raise RuntimeError('P2 fixture must contain one reference P slice')
                b = vector.Bits(vector.rbsp(p2s[0][1:]))
                mb, kind, pid = b.ue(), b.ue(), b.ue()
                frame_num = b.get(sps['log2_max_frame_num_minus4'] + 4)
                if mb != 0 or kind % 5 != 0 or pid != pps['pic_parameter_set_id'] or frame_num != 2:
                    raise RuntimeError('unsupported P2-frame fixture')
                p2_slice = b'\0\0\0\1' + p2s[0]
                vals['V_P2_REF'] = (p2s[0][0] >> 5) & 3
            vals.setdefault('V_P2_REF', 0)
            vals['V_HAS_P2'] = int(bool(p2_slice))
            seq_data = bytearray()
            seq_offsets, seq_lengths, seq_refs = [], [], []
            if args.sequence:
                image_size = dimensions[0] * dimensions[1] * 3 // 2
                raw_size = args.sequence_nv12.stat().st_size
                if raw_size % image_size or not 4 <= raw_size // image_size <= 300:
                    raise RuntimeError('sequence reference must contain 4 to 300 NV12 frames')
                seq_count = raw_size // image_size
                snals = list(vector.annex_b(args.sequence.read_bytes()))
                ps = [n for n in snals if n[0] & 31 == 1]
                if len(ps) < seq_count - 1 or b'\0\0\0\1' + ps[0] != p_slice or b'\0\0\0\1' + ps[1] != p2_slice:
                    raise RuntimeError('sequence does not match initial P fixtures')
                for index, nal in enumerate(ps[2:seq_count - 1], 3):
                    b = vector.Bits(vector.rbsp(nal[1:]))
                    if (not nal[0] & 0x60 or b.ue() != 0 or b.ue() % 5 != 0 or
                            b.ue() != pps['pic_parameter_set_id'] or b.get(4) != index % 16):
                        raise RuntimeError(f'unsupported sequence frame {index}')
                    packet = b'\0\0\0\1' + nal
                    seq_offsets.append(len(seq_data))
                    seq_lengths.append(len(packet))
                    seq_data.extend(packet)
                    seq_refs.append((nal[0] >> 5) & 3)
                with args.sequence_nv12.open('rb') as raw:
                    seq_crcs = [zlib.crc32(raw.read(image_size)) for _ in range(seq_count)]
            else:
                seq_count, seq_crcs = 0, [0]
            vals['V_SEQ_COUNT'] = seq_count
            vals['V_SEQ_MAX'] = max(seq_lengths, default=1)
            vals['V_P_DECODE'] = int(bool(args.p_reference))
            vals['V_CORRUPT_REF'] = int(args.corrupt_reference)
            p_reference = args.p_reference.resolve() if args.p_reference else reference
            p2_reference = args.p2_reference.resolve() if args.p2_reference else reference
            if p_reference.stat().st_size != dimensions[0] * dimensions[1] * 3 // 2:
                raise RuntimeError('P reference size does not match dimensions')
            if p2_reference.stat().st_size != dimensions[0] * dimensions[1] * 3 // 2:
                raise RuntimeError('P2 reference size does not match dimensions')
            header = ''.join(f'#define {k} ({v})\n' for k, v in vals.items())
            header += 'static const unsigned char vector_slice[] = {' + ','.join(str(x) for x in idr) + '};\n'
            header += 'static const unsigned char vector_p[] = {' + (','.join(str(x) for x in p_slice) or '0') + '};\n'
            header += 'static const unsigned char vector_p2[] = {' + (','.join(str(x) for x in p2_slice) or '0') + '};\n'
            seq_path = Path(tmp) / 'sequence.bin'
            seq_path.write_bytes(seq_data or b'\0')
            header += f'extern const unsigned char vector_sequence[];\n__asm__(".pushsection .rodata\\n.global vector_sequence\\nvector_sequence:\\n.incbin \\"{seq_path}\\"\\n.popsection\\n");\n'
            for name, values in [('offsets', seq_offsets), ('lengths', seq_lengths),
                                 ('refs', seq_refs), ('crcs', seq_crcs)]:
                header += f'static const unsigned vector_seq_{name}[] = {{' + (','.join(str(x) for x in values) or '0') + '};\n'
            header += f'extern const unsigned char vector_reference[];\n__asm__(".pushsection .rodata\\n.global vector_reference\\nvector_reference:\\n.incbin \\"{reference}\\"\\n.popsection\\n");\n'
            header += f'extern const unsigned char vector_p_reference[];\n__asm__(".pushsection .rodata\\n.global vector_p_reference\\nvector_p_reference:\\n.incbin \\"{p_reference}\\"\\n.popsection\\n");\n'
            header += f'extern const unsigned char vector_p2_reference[];\n__asm__(".pushsection .rodata\\n.global vector_p2_reference\\nvector_p2_reference:\\n.incbin \\"{p2_reference}\\"\\n.popsection\\n");\n'
            (Path(tmp) / 'vector.h').write_text(header)
            sysheader = Path(tmp) / 'sys'
            sysheader.mkdir()
            (sysheader / 'time.h').write_text('struct timeval { long tv_sec; long tv_usec; };\n'
                                             'struct timespec { long tv_sec; long tv_nsec; };\n')
            binary = Path(tmp) / 'vdec-request-test'
            subprocess.run([args.cc, '-Os', '-Wall', '-Wextra', '-ffreestanding', '-fno-builtin',
                            '-fno-stack-protector', '-nostdlib', '-static', '-Wl,-e,_start',
                            '-I', tmp, '-I/usr/include/aarch64-linux-gnu', '-I/usr/include',
                            '-o', str(binary), str(ROOT / 'scripts/vdec-request-test.c')], check=True)
            request_probe = binary.read_bytes()
        initrd.write_bytes(base.read_bytes() + overlay(args.modules, probe, request_probe))
        desktop.SETENV = ('setenv bootargs console=ttyS2,1500000 rdinit=/init loglevel=4' +
                          (' vdec.disk=1' if args.image else '') +
                          (' vdec.ioctl=1' if args.ioctl else '') +
                          (' vdec.request=1' if args.request else ''))
        desktop.BOOTI = f'booti 0x02000000 0x0b000000:{initrd.stat().st_size:#x} 0x0a100000'
        machine = 'rk3588,pcie-link=off' + (',vdec0-power=on' if args.vdec0_power else '')
        cmd = [str(Path(args.qemu).resolve()), '-M', machine,
               '-display', 'none', '-serial', 'stdio', '-monitor', 'none',
               '-bios', str(bl31), '-dtb', str(uboot_dtb),
               '-device', f'loader,file={uboot},addr=0x800000,force-raw=on',
               '-device', f'loader,file={kernel},addr=0x02000000,force-raw=on',
               '-device', f'loader,file={Path(args.dtb).resolve()},addr=0x0a100000,force-raw=on',
               '-device', f'loader,file={initrd},addr=0x0b000000,force-raw=on']
        if args.image:
            cmd += ['-snapshot', '-drive', f'if=sd,format=raw,file={args.image.resolve()}']
        if args.request:
            cmd += ['-trace', 'rk3588_vdec_dma_probe', '-D', str(Path(tmp) / 'vdec-trace.log')]
        if args.p_frame:
            cmd += ['-trace', 'rk3588_vdec_reference']
        env = os.environ.copy()
        if args.qemu.endswith('.AppImage'):
            env.setdefault('APPIMAGE_EXTRACT_AND_RUN', '1')
        guest = desktop.Guest(cmd, env)
        try:
            error = desktop.dance(guest)
            if error:
                raise RuntimeError(error)
            start = time.monotonic()
            if not guest.wait(b'VDEC_PROBE_DONE', args.timeout):
                raise RuntimeError('decoder discovery did not finish')
            output = guest.since(0)
            if args.expect_vdec0:
                if (b'device: /sys/bus/platform/devices/fdc38100.video-codec\r\n'
                        b'driver: /sys/bus/platform/drivers/rkvdec' not in output or
                        b'device: /sys/devices/platform/fdc38100.video-codec' not in output):
                    raise RuntimeError('VDEC0 did not bind and expose a video node')
                if b"failed to get mem status 'rkvdec0'" in output:
                    raise RuntimeError('VDEC0 memory power transition failed')
                print('PASS: VDEC0 bound and exposed a video node', flush=True)
            if args.ioctl:
                if b'VDEC_IOCTL_H264_SLICE_PRESENT' not in output or b'VDEC_IOCTL_FAILED' in output:
                    raise RuntimeError('VDEC0 H.264 format or request probe failed')
                if b'VDEC_REQUEST_ALLOC_REINIT_OK' not in output:
                    raise RuntimeError('VDEC0 media request allocation did not complete')
                for marker in (b'VDEC_EMPTY_REQUEST_REJECTED', b'VDEC_OUTPUT_BUFFER_ALLOC_OK',
                               b'VDEC_CAPTURE_BUFFER_ALLOC_OK'):
                    if marker not in output:
                        raise RuntimeError(f'VDEC0 request/buffer probe missing {marker.decode()}')
                print('PASS: VDEC0 H.264 formats, media requests and buffer queues', flush=True)
            if args.request:
                marker = b'VDEC_REQUEST_DECODE_MATCH' if args.decode else b'VDEC_REQUEST_EXPECTED_ERROR'
                if marker not in output or b'VDEC_REQUEST_TEST_FAILED' in output:
                    raise RuntimeError('H.264 request did not return the expected result')
                trace = (Path(tmp) / 'vdec-trace.log').read_text()
                expected_dma = f'len=16 result=0 sum={sum(idr[:16])}'
                if not any('rk3588_vdec_dma_probe' in line and expected_dma in line
                           for line in trace.splitlines()):
                    raise RuntimeError(f'H.264 request DMA data mismatch; expected {expected_dma}: {trace}')
                if args.p_frame:
                    expected_p_dma = f'len=16 result=0 sum={sum(p_slice[:16])}'
                    p_marker = b'VDEC_P_DECODE_MATCH' if args.p_reference else b'VDEC_P_EXPECTED_ERROR'
                    if (p_marker not in output or
                            not any('rk3588_vdec_dma_probe' in line and expected_p_dma in line
                                    for line in trace.splitlines())):
                        raise RuntimeError('P-frame did not reach VDEC and return the expected error')
                    refs = re.findall(r'rk3588_vdec_reference dst=0x([0-9a-f]+) ref=0x([0-9a-f]+) info=0x([0-9a-f]+)', trace)
                    if len(refs) != (seq_count if args.sequence else (3 if args.p2_frame else 2)):
                        raise RuntimeError(f'unexpected number of VDEC reference traces: {trace}')
                    first_dst, _, _ = (int(x, 16) for x in refs[0])
                    second_dst, second_ref, info = (int(x, 16) for x in refs[1])
                    if first_dst != second_ref or second_dst == second_ref or info & 0xe != 0xe:
                        raise RuntimeError(f'P-frame reference metadata mismatch: {refs}')
                    if args.p2_frame:
                        third_dst, third_ref, info = (int(x, 16) for x in refs[2])
                        p2_dma = f'len=16 result=0 sum={sum(p2_slice[:16])}'
                        if (third_ref != second_dst or third_dst == third_ref or info & 0xe != 0xe or
                                b'VDEC_P2_DECODE_MATCH' not in output or
                                not any('rk3588_vdec_dma_probe' in line and p2_dma in line for line in trace.splitlines())):
                            raise RuntimeError(f'P2 decode or reference metadata mismatch: {refs}')
                        print('PASS: second P-frame references first P and matches NV12 reference', flush=True)
                    if args.sequence:
                        if b'VDEC_SEQUENCE_MATCH' not in output:
                            raise RuntimeError('sequence did not match reference')
                        timing = re.search(rb'VDEC_SEQUENCE_ELAPSED_MS (\d+)', output)
                        if not timing or not int(timing.group(1)):
                            raise RuntimeError('sequence timing missing or invalid')
                        elapsed_ms = int(timing.group(1))
                        for i in range(3, seq_count):
                            dst_i, ref_i, info_i = (int(x, 16) for x in refs[i])
                            if ref_i != int(refs[i - 1][0], 16) or ref_i == dst_i or info_i & 0xe != 0xe:
                                raise RuntimeError(f'sequence reference mismatch at frame {i}')
                        print(f'PASS: {seq_count} sequential pictures match NV12 references '
                              '(first 3 byte-for-byte, remainder CRC32)', flush=True)
                        print(f'Decode and guest verification: {elapsed_ms} ms '
                              f'({seq_count * 1000 / elapsed_ms:.1f} pictures/s); no display', flush=True)
                    print('PASS: P-frame references IDR capture buffer and ' +
                          ('matches NV12 reference' if args.p_reference else 'returns controlled error'), flush=True)
                print('PASS: H.264 frame matches NV12 reference' if args.decode else
                      'PASS: H.264 request reached decoder and returned controlled error', flush=True)
            print(f'\nPASS: test completed in {time.monotonic() - start:.1f}s' +
                  ('' if args.decode else '; no frame decoded'), flush=True)
        finally:
            guest.stop()


if __name__ == '__main__':
    main()
