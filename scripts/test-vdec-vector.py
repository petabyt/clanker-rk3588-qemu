#!/usr/bin/env python3
"""Generate and validate the restricted single-IDR H.264 VDEC fixture."""
import importlib.util
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location('vector', root / 'scripts/inspect-vdec-vector.py')
vector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vector)

with tempfile.TemporaryDirectory(prefix='vdec-vector-', dir='/tmp/opencode') as tmp:
    tmp = Path(tmp)
    flags = subprocess.check_output(['pkg-config', '--cflags', '--libs',
                                     'libavcodec', 'libavutil'], text=True).split()
    exe = tmp / 'make-vector'
    subprocess.run(['cc', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(root / 'scripts/make-vdec-vector.c'), '-o', str(exe),
                    *flags], check=True)
    for n in (1, 2):
        subprocess.run([str(exe), str(tmp / f'frame{n}.h264'),
                        str(tmp / f'ref{n}.yuv')], check=True)
    data = (tmp / 'frame1.h264').read_bytes()
    assert data == (tmp / 'frame2.h264').read_bytes()
    assert (tmp / 'ref1.yuv').read_bytes() == (tmp / 'ref2.yuv').read_bytes()
    assert len((tmp / 'ref1.yuv').read_bytes()) == 64 * 64 * 3 // 2
    nals = list(vector.annex_b(data))
    types = [n[0] & 31 for n in nals]
    assert types == [7, 8, 6, 5]
    sps = vector.parse_sps(nals[0])
    pps = vector.parse_pps(nals[1], sps)
    pic = vector.parse_idr(nals[3], sps, pps)
    assert (sps['profile_idc'], sps['constraint_set_flags'], sps['pic_order_cnt_type'],
            sps['pic_width_in_mbs_minus1'], sps['pic_height_in_map_units_minus1']) == (66, 3, 2, 3, 3)
    assert (pps['pic_init_qp_minus26'], pps['deblocking_filter_control_present_flag']) == (-3, 1)
    assert (pic['frame_num'], pic['idr_pic_id'], pic['nal_ref_idc']) == (0, 0, 3)
    try:
        vector.parse_sps(nals[0][:3])
    except ValueError:
        pass
    else:
        raise AssertionError('truncated SPS accepted')
    print('PASS: reproducible H.264 keyframe, software reference and parsed controls')
