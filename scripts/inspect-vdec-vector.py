#!/usr/bin/env python3
"""Extract baseline H.264 parameters for the single-IDR VDEC test vector.

This is deliberately not a general H.264 parser: unsupported stream features
are rejected rather than silently producing incorrect V4L2 controls.
"""
import argparse
import json
from pathlib import Path
import re


class Bits:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def get(self, n):
        if n < 0 or self.pos + n > len(self.data) * 8:
            raise ValueError('truncated H.264 RBSP')
        value = 0
        for _ in range(n):
            value = (value << 1) | ((self.data[self.pos // 8] >> (7 - self.pos % 8)) & 1)
            self.pos += 1
        return value

    def ue(self):
        zeros = 0
        while not self.get(1):
            zeros += 1
            if zeros > 31:
                raise ValueError('Exp-Golomb value too large')
        return (1 << zeros) - 1 + self.get(zeros)

    def se(self):
        value = self.ue()
        return (value + 1) // 2 if value & 1 else -(value // 2)


def rbsp(data):
    result = bytearray()
    zeros = 0
    i = 0
    while i < len(data):
        value = data[i]
        if zeros >= 2 and value == 3:
            if i + 1 >= len(data) or data[i + 1] > 3:
                raise ValueError('invalid emulation prevention byte')
            zeros = 0
            i += 1
            continue
        result.append(value)
        zeros = zeros + 1 if value == 0 else 0
        i += 1
    return bytes(result)


def annex_b(data):
    starts = list(re.finditer(b'\x00\x00(?:\x00)?\x01', data))
    if not starts or any(data[:starts[0].start()]):
        raise ValueError('expected Annex B NAL units')
    for i, start in enumerate(starts):
        end = starts[i + 1].start() if i + 1 < len(starts) else len(data)
        if start.end() >= end:
            raise ValueError('empty NAL unit')
        yield data[start.end():end]


def parse_sps(nal):
    b = Bits(rbsp(nal[1:]))
    profile, constraint, level, sps_id = b.get(8), b.get(8), b.get(8), b.ue()
    if profile != 66 or sps_id > 31 or constraint & 3:
        raise ValueError('only baseline SPS with valid reserved bits supported')
    frame_num_bits = b.ue() + 4
    poc_type = b.ue()
    if frame_num_bits > 16 or poc_type not in (0, 2):
        raise ValueError('unsupported frame-number or POC configuration')
    poc_bits = b.ue() + 4 if poc_type == 0 else 0
    if poc_bits > 16:
        raise ValueError('unsupported POC width')
    refs, gaps = b.ue(), b.get(1)
    width, height = b.ue() + 1, b.ue() + 1
    progressive = b.get(1)
    if not progressive:
        raise ValueError('interlaced SPS unsupported')
    direct = b.get(1)
    crop = [b.ue() for _ in range(4)] if b.get(1) else [0, 0, 0, 0]
    flags = sum(((constraint >> (7 - i)) & 1) << i for i in range(6))
    return dict(profile_idc=profile, constraint_set_flags=flags,
                level_idc=level, seq_parameter_set_id=sps_id,
                chroma_format_idc=1, log2_max_frame_num_minus4=frame_num_bits - 4,
                pic_order_cnt_type=poc_type,
                log2_max_pic_order_cnt_lsb_minus4=poc_bits - 4 if poc_bits else 0,
                max_num_ref_frames=refs, gaps_in_frame_num_value_allowed=gaps,
                pic_width_in_mbs_minus1=width - 1,
                pic_height_in_map_units_minus1=height - 1,
                frame_mbs_only_flag=progressive, direct_8x8_inference_flag=direct,
                crop=crop)


def parse_pps(nal, sps):
    b = Bits(rbsp(nal[1:]))
    pps, seq = b.ue(), b.ue()
    entropy, bottom, groups = b.get(1), b.get(1), b.ue()
    if seq != sps['seq_parameter_set_id'] or groups:
        raise ValueError('unsupported PPS: SPS mismatch or slice groups')
    l0, l1 = b.ue(), b.ue()
    weighted, bipred = b.get(1), b.get(2)
    qp, qs, chroma = b.se(), b.se(), b.se()
    deblock, constrained, redundant = b.get(1), b.get(1), b.get(1)
    if b.get(1) != 1 or any(b.get(1) for _ in range(len(b.data) * 8 - b.pos)):
        raise ValueError('PPS extensions are not supported')
    return dict(pic_parameter_set_id=pps, seq_parameter_set_id=seq,
                num_slice_groups_minus1=groups,
                num_ref_idx_l0_default_active_minus1=l0,
                num_ref_idx_l1_default_active_minus1=l1,
                weighted_bipred_idc=bipred, pic_init_qp_minus26=qp,
                pic_init_qs_minus26=qs, chroma_qp_index_offset=chroma,
                second_chroma_qp_index_offset=chroma,
                entropy_coding_mode_flag=entropy,
                bottom_field_pic_order_in_frame_present_flag=bottom,
                weighted_pred_flag=weighted,
                deblocking_filter_control_present_flag=deblock,
                constrained_intra_pred_flag=constrained,
                redundant_pic_cnt_present_flag=redundant)


def parse_idr(nal, sps, pps):
    if nal[0] & 0x80 or (nal[0] & 31) != 5:
        raise ValueError('expected an IDR slice')
    b = Bits(rbsp(nal[1:]))
    first, kind, pps_id = b.ue(), b.ue(), b.ue()
    if first != 0 or kind % 5 != 2 or pps_id != pps['pic_parameter_set_id']:
        raise ValueError('expected first I slice referencing the parsed PPS')
    frame_num = b.get(sps['log2_max_frame_num_minus4'] + 4)
    idr_pic_id = b.ue()
    poc = b.get(sps['log2_max_pic_order_cnt_lsb_minus4'] + 4) if sps['pic_order_cnt_type'] == 0 else 0
    if pps['bottom_field_pic_order_in_frame_present_flag']:
        raise ValueError('bottom-field POC unsupported')
    if pps['redundant_pic_cnt_present_flag']:
        raise ValueError('redundant pictures unsupported')
    return dict(first_mb_in_slice=first, slice_type=kind, pic_parameter_set_id=pps_id,
                frame_num=frame_num, idr_pic_id=idr_pic_id, pic_order_cnt_lsb=poc,
                nal_ref_idc=(nal[0] >> 5) & 3)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('image', type=Path)
    args = parser.parse_args()
    nals = list(annex_b(args.image.read_bytes()))
    types = [n[0] & 31 for n in nals]
    if (types.count(7) != 1 or types.count(8) != 1 or types.count(5) != 1 or
            any(t in (1, 2, 3, 4, 19, 20, 21) for t in types)):
        raise ValueError(f'expected exactly one SPS, PPS and IDR slice: {types}')
    sps = parse_sps(nals[types.index(7)])
    pps = parse_pps(nals[types.index(8)], sps)
    picture = parse_idr(nals[types.index(5)], sps, pps)
    print(json.dumps(dict(nal_types=types, sps=sps, pps=pps, picture=picture), indent=2))


if __name__ == '__main__':
    main()
