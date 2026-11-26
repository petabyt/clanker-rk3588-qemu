#!/usr/bin/env python3
"""Validate the restricted I/P Annex B fixture before building V4L2 requests.

This checks only fields needed to identify pictures, not the full slice header
or reference-list operations. It does not establish that a decoder supports
the stream or that its reference list uses the previous frame.
"""
import argparse
import importlib.util
from pathlib import Path

spec = importlib.util.spec_from_file_location('vector', Path(__file__).with_name('inspect-vdec-vector.py'))
vector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(vector)


def inspect(path):
    nals = list(vector.annex_b(path.read_bytes()))
    groups = []
    for nal in nals:
        kind = nal[0] & 31
        if kind == 9:
            groups.append([])
        if not groups:
            raise ValueError('fixture must start with an access-unit delimiter')
        groups[-1].append(nal)
    if not groups:
        raise ValueError('no access units')
    types = [n[0] & 31 for n in groups[0]]
    if types.count(7) != 1 or types.count(8) != 1 or types.count(5) != 1:
        raise ValueError('first access unit must contain SPS, PPS and IDR')
    sps = vector.parse_sps(groups[0][types.index(7)])
    pps = vector.parse_pps(groups[0][types.index(8)], sps)
    first = vector.parse_idr(groups[0][types.index(5)], sps, pps)
    if first['frame_num'] != 0 or sps['pic_order_cnt_type'] != 2:
        raise ValueError('fixture requires IDR frame number zero and POC type 2')
    if sps['max_num_ref_frames'] != 1 or sps['gaps_in_frame_num_value_allowed']:
        raise ValueError('fixture requires one reference and no frame number gaps')
    modulus = 1 << (sps['log2_max_frame_num_minus4'] + 4)
    for index, group in enumerate(groups[1:], 1):
        slices = [nal for nal in group if (nal[0] & 31) in (1, 2, 3, 4, 5, 19, 20, 21)]
        if len(slices) != 1 or slices[0][0] & 31 != 1 or not slices[0][0] & 0x60:
            raise ValueError(f'frame {index}: expected one reference P slice')
        bits = vector.Bits(vector.rbsp(slices[0][1:]))
        first_mb, slice_type, pps_id = bits.ue(), bits.ue(), bits.ue()
        frame_num = bits.get(sps['log2_max_frame_num_minus4'] + 4)
        if (first_mb != 0 or slice_type % 5 != 0 or
                pps_id != pps['pic_parameter_set_id'] or frame_num != index % modulus):
            raise ValueError(f'frame {index}: unexpected slice header or frame number')
    print(f'PASS: {len(groups)} access units: 1 IDR and {len(groups) - 1} reference P pictures; '
          f'frame_num modulus {modulus}')
    print('Slice reference-list modifications and decoded pictures were not checked.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('stream', type=Path)
    inspect(parser.parse_args().stream)
