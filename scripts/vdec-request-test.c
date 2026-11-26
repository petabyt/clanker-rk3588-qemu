/* Freestanding AArch64 V4L2 request test for the generated baseline IDR.
 * The generated vector.h supplies a single Annex-B IDR slice and parameters.
 * Depending on the fixture, expect either controlled error or exact NV12.
 */
#include <linux/videodev2.h>
#include <linux/media.h>
#include "vector.h"

typedef unsigned long u64;

void *memset(void *dst, int c, u64 n)
{
    unsigned char *p = dst;
    while (n--) *p++ = c;
    return dst;
}

static long call(long nr, long a, long b, long c, long d, long e, long f)
{
    register long x0 __asm__("x0") = a, x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c, x3 __asm__("x3") = d;
    register long x4 __asm__("x4") = e, x5 __asm__("x5") = f;
    register long x8 __asm__("x8") = nr;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5), "r"(x8) : "memory", "cc");
    return x0;
}

static void print(const char *s)
{
    u64 n = 0;
    while (s[n]) n++;
    call(64, 1, (long)s, n, 0, 0, 0);
}

static void hex(unsigned n)
{
    char s[] = "0x00000000\n";
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 8; i++) { s[9-i] = digits[n & 15]; n >>= 4; }
    print(s);
}

static void decimal(u64 n)
{
    char buf[24];
    unsigned pos = sizeof(buf) - 1;
    buf[pos] = 0;
    do { buf[--pos] = '0' + n % 10; n /= 10; } while (n);
    print(buf + pos);
}

static int check(const char *what, long result)
{
    if (result < 0) {
        print("VDEC_REQUEST_FAIL "); print(what); print(" errno="); hex(-result);
        return 0;
    }
    return 1;
}

static unsigned frame_crc(const unsigned char *p, unsigned stride, unsigned height)
{
    unsigned table[256], crc = ~0u;
    for (unsigned i = 0; i < 256; i++) {
        unsigned v = i;
        for (unsigned j = 0; j < 8; j++) v = (v >> 1) ^ (0xedb88320u & -(v & 1));
        table[i] = v;
    }
    for (unsigned y = 0; y < V_HEIGHT; y++)
        for (unsigned x = 0; x < V_WIDTH; x++)
            crc = table[(crc ^ p[y * stride + x]) & 255] ^ (crc >> 8);
    p += stride * height;
    for (unsigned y = 0; y < V_HEIGHT / 2; y++)
        for (unsigned x = 0; x < V_WIDTH; x++)
            crc = table[(crc ^ p[y * stride + x]) & 255] ^ (crc >> 8);
    return ~crc;
}

static int run(void)
{
    long video = call(56, -100, (long)"/dev/video0", 2 | 04000, 0, 0, 0);
    long media = call(56, -100, (long)"/dev/media0", 2, 0, 0, 0);
    int request = -1;
    struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
    struct v4l2_ctrl_h264_sps sps = {
        .profile_idc = V_SPS_PROFILE, .constraint_set_flags = V_SPS_CONSTRAINT,
        .level_idc = V_SPS_LEVEL, .seq_parameter_set_id = V_SPS_ID,
        .chroma_format_idc = 1, .log2_max_frame_num_minus4 = V_SPS_FRAME_BITS,
        .pic_order_cnt_type = V_SPS_POC_TYPE,
        .log2_max_pic_order_cnt_lsb_minus4 = V_SPS_POC_BITS,
        .max_num_ref_frames = V_SPS_REFS,
        .pic_width_in_mbs_minus1 = V_SPS_WIDTH,
        .pic_height_in_map_units_minus1 = V_SPS_HEIGHT,
        .flags = V_SPS_FLAGS,
    };
    struct v4l2_ctrl_h264_pps pps = {
        .pic_parameter_set_id = V_PPS_ID, .seq_parameter_set_id = V_SPS_ID,
        .num_ref_idx_l0_default_active_minus1 = V_PPS_L0,
        .num_ref_idx_l1_default_active_minus1 = V_PPS_L1,
        .pic_init_qp_minus26 = V_PPS_QP, .pic_init_qs_minus26 = V_PPS_QS,
        .chroma_qp_index_offset = V_PPS_CHROMA,
        .second_chroma_qp_index_offset = V_PPS_CHROMA,
        .weighted_bipred_idc = V_PPS_BIPRED, .flags = V_PPS_FLAGS,
    };
    struct v4l2_ctrl_h264_decode_params decode = {
        .nal_ref_idc = V_PIC_REF, .frame_num = V_PIC_FRAME,
        .idr_pic_id = V_PIC_IDR, .pic_order_cnt_lsb = V_PIC_POC,
        .flags = V4L2_H264_DECODE_PARAM_FLAG_IDR_PIC,
    };
    struct v4l2_ext_control controls[3] = {
        { .id = V4L2_CID_STATELESS_H264_SPS, .size = sizeof(sps), .ptr = &sps },
        { .id = V4L2_CID_STATELESS_H264_PPS, .size = sizeof(pps), .ptr = &pps },
        { .id = V4L2_CID_STATELESS_H264_DECODE_PARAMS, .size = sizeof(decode), .ptr = &decode },
    };
    struct v4l2_ext_controls ext = { .which = V4L2_CTRL_WHICH_REQUEST_VAL,
                                     .count = 3, .controls = controls };
    struct v4l2_plane srcplane = { 0 }, dstplane = { 0 };
    struct v4l2_buffer src = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                               .memory = V4L2_MEMORY_MMAP, .length = 1,
                               .m.planes = &srcplane };
    struct v4l2_buffer dst = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                               .memory = V4L2_MEMORY_MMAP, .length = 1,
                               .m.planes = &dstplane };
    long mapped, capture = 0, ret;
    struct timespec sequence_start = {0}, sequence_end = {0};
    if (!check("open-video", video) || !check("open-media", media)) return 1;
    fmt.fmt.pix_mp.width = V_WIDTH;
    fmt.fmt.pix_mp.height = V_HEIGHT;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_H264_SLICE;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = sizeof(vector_slice) < 4096 ? 4096 : sizeof(vector_slice);
    if (V_HAS_P && sizeof(vector_p) > fmt.fmt.pix_mp.plane_fmt[0].sizeimage)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = sizeof(vector_p);
    if (V_HAS_P2 && sizeof(vector_p2) > fmt.fmt.pix_mp.plane_fmt[0].sizeimage)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = sizeof(vector_p2);
    if (V_SEQ_COUNT && V_SEQ_MAX > fmt.fmt.pix_mp.plane_fmt[0].sizeimage)
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage = V_SEQ_MAX;
    if (!check("output-format", call(29, video, VIDIOC_S_FMT, (long)&fmt, 0, 0, 0))) return 1;
    struct v4l2_format capture_fmt = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    if (!check("capture-format", call(29, video, VIDIOC_G_FMT, (long)&capture_fmt, 0, 0, 0))) return 1;
    for (int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
         type <= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; type++) {
        unsigned count = V_HAS_P && type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ? 2 : 1;
        struct v4l2_requestbuffers rb = { .count = count, .type = type, .memory = V4L2_MEMORY_MMAP };
        if (!check("reqbufs", call(29, video, VIDIOC_REQBUFS, (long)&rb, 0, 0, 0)) || rb.count < count) return 1;
    }
    if (!check("query-output", call(29, video, VIDIOC_QUERYBUF, (long)&src, 0, 0, 0))) return 1;
    if (!check("query-capture", call(29, video, VIDIOC_QUERYBUF, (long)&dst, 0, 0, 0))) return 1;
    if (V_EXPECT_DECODE) {
        capture = call(222, 0, dstplane.length, 3, 1, video, dstplane.m.mem_offset);
        if (!check("mmap-capture", capture)) return 1;
    }
    if (srcplane.length < sizeof(vector_slice)) { print("VDEC_REQUEST_FAIL output-too-small\n"); return 1; }
    mapped = call(222, 0, srcplane.length, 3, 1, video, srcplane.m.mem_offset);
    if (!check("mmap-output", mapped)) return 1;
    for (u64 i = 0; i < sizeof(vector_slice); i++) ((unsigned char *)mapped)[i] = vector_slice[i];
    if (!check("request-alloc", call(29, media, MEDIA_IOC_REQUEST_ALLOC, (long)&request, 0, 0, 0))) return 1;
    ext.request_fd = request;
    if (!check("set-controls", call(29, video, VIDIOC_S_EXT_CTRLS, (long)&ext, 0, 0, 0))) {
        print("VDEC_REQUEST_CONTROL_INDEX "); hex(ext.error_idx); return 1;
    }
    if (!check("queue-capture", call(29, video, VIDIOC_QBUF, (long)&dst, 0, 0, 0))) return 1;
    srcplane.bytesused = sizeof(vector_slice);
    src.flags = V4L2_BUF_FLAG_REQUEST_FD;
    src.timestamp.tv_sec = 1;
    src.request_fd = request;
    if (!check("queue-output", call(29, video, VIDIOC_QBUF, (long)&src, 0, 0, 0))) return 1;
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (!check("streamon-capture", call(29, video, VIDIOC_STREAMON, (long)&type, 0, 0, 0))) return 1;
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (!check("streamon-output", call(29, video, VIDIOC_STREAMON, (long)&type, 0, 0, 0))) return 1;
    if (V_SEQ_COUNT && !check("sequence-clock-start", call(113, 1, (long)&sequence_start, 0, 0, 0, 0))) return 1;
    if (!check("request-queue", call(29, request, MEDIA_REQUEST_IOC_QUEUE, 0, 0, 0, 0))) return 1;
    print("VDEC_REQUEST_SUBMITTED\n");
    /* Wait on the request with ppoll; avoid blocking the initramfs forever. */
    struct { int fd; short events, revents; } pollfd = { .fd = request, .events = 2 };
    struct timespec timeout = { .tv_sec = 10 };
    ret = call(73, (long)&pollfd, 1, (long)&timeout, 0, 0, 0);
    if (!check("poll", ret) || ret == 0) { print("VDEC_REQUEST_TIMEOUT\n"); return 1; }
    dstplane = (struct v4l2_plane){0};
    if (!check("dequeue-capture", call(29, video, VIDIOC_DQBUF, (long)&dst, 0, 0, 0))) return 1;
    if (V_EXPECT_DECODE) {
        unsigned stride = capture_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
        unsigned height = capture_fmt.fmt.pix_mp.height;
        unsigned char *p = (unsigned char *)capture;
        if ((dst.flags & V4L2_BUF_FLAG_ERROR) || stride < V_WIDTH || height < V_HEIGHT ||
            (u64)stride * (height + V_HEIGHT / 2) > dstplane.length) {
            print("VDEC_CAPTURE_FLAGS "); hex(dst.flags);
            print("VDEC_CAPTURE_STRIDE "); hex(stride);
            print("VDEC_CAPTURE_HEIGHT "); hex(height);
            print("VDEC_CAPTURE_LENGTH "); hex(dstplane.length);
            print("VDEC_REQUEST_FAIL bad-capture\n"); return 1;
        }
        for (unsigned y = 0; y < V_HEIGHT; y++) {
            for (unsigned x = 0; x < V_WIDTH; x++) {
                if (p[y * stride + x] != vector_reference[y * V_WIDTH + x]) {
                    print("VDEC_REQUEST_FAIL luma-mismatch\n"); return 1;
                }
            }
        }
        for (unsigned y = 0; y < V_HEIGHT / 2; y++) {
            for (unsigned x = 0; x < V_WIDTH / 2; x++) {
                if (p[stride * height + y * stride + 2 * x] != vector_reference[V_WIDTH * V_HEIGHT + y * (V_WIDTH / 2) + x] ||
                    p[stride * height + y * stride + 2 * x + 1] != vector_reference[V_WIDTH * V_HEIGHT * 5 / 4 + y * (V_WIDTH / 2) + x]) {
                    print("VDEC_REQUEST_FAIL chroma-mismatch\n"); return 1;
                }
            }
        }
        print("VDEC_REQUEST_DECODE_MATCH\n");
        if (V_HAS_P) {
            if (V_CORRUPT_REF) p[0] ^= 1;
            srcplane = (struct v4l2_plane){0};
            if (!check("dequeue-output", call(29, video, VIDIOC_DQBUF, (long)&src, 0, 0, 0))) return 1;
            if (!check("reinit-request", call(29, request, MEDIA_REQUEST_IOC_REINIT, 0, 0, 0, 0))) return 1;
            if (sizeof(vector_p) > srcplane.length) { print("VDEC_REQUEST_FAIL p-output-too-small\n"); return 1; }
            for (u64 i = 0; i < sizeof(vector_p); i++) ((unsigned char *)mapped)[i] = vector_p[i];
            decode = (struct v4l2_ctrl_h264_decode_params){
                .nal_ref_idc = V_P_REF, .frame_num = 1,
                .top_field_order_cnt = 2, .bottom_field_order_cnt = 2,
                .flags = V4L2_H264_DECODE_PARAM_FLAG_PFRAME,
            };
            decode.dpb[0].reference_ts = 1000000000ULL;
            decode.dpb[0].fields = V4L2_H264_FRAME_REF;
            decode.dpb[0].flags = V4L2_H264_DPB_ENTRY_FLAG_VALID | V4L2_H264_DPB_ENTRY_FLAG_ACTIVE;
            if (!check("p-set-controls", call(29, video, VIDIOC_S_EXT_CTRLS, (long)&ext, 0, 0, 0))) return 1;
            dst = (struct v4l2_buffer){ .index = 1, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                        .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &dstplane };
            dstplane = (struct v4l2_plane){0};
            if (!check("p-query-capture", call(29, video, VIDIOC_QUERYBUF, (long)&dst, 0, 0, 0))) return 1;
            long pcapture = call(222, 0, dstplane.length, 3, 1, video, dstplane.m.mem_offset);
            if (!check("p-mmap-capture", pcapture)) return 1;
            if (!check("p-queue-capture", call(29, video, VIDIOC_QBUF, (long)&dst, 0, 0, 0))) return 1;
            src = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                       .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &srcplane,
                                       .flags = V4L2_BUF_FLAG_REQUEST_FD, .request_fd = request };
            src.timestamp.tv_sec = 2;
            srcplane = (struct v4l2_plane){ .bytesused = sizeof(vector_p) };
            if (!check("p-queue-output", call(29, video, VIDIOC_QBUF, (long)&src, 0, 0, 0))) return 1;
            if (!check("p-request-queue", call(29, request, MEDIA_REQUEST_IOC_QUEUE, 0, 0, 0, 0))) return 1;
            pollfd.revents = 0;
            timeout = (struct timespec){ .tv_sec = 10 };
            ret = call(73, (long)&pollfd, 1, (long)&timeout, 0, 0, 0);
            if (!check("p-poll", ret) || !ret) { print("VDEC_P_TIMEOUT\n"); return 1; }
            dstplane = (struct v4l2_plane){0};
            if (!check("p-dequeue-capture", call(29, video, VIDIOC_DQBUF, (long)&dst, 0, 0, 0))) return 1;
            if (V_P_DECODE) {
                unsigned char *q = (unsigned char *)pcapture;
                if ((dst.flags & V4L2_BUF_FLAG_ERROR) || (u64)stride * (height + V_HEIGHT / 2) > dstplane.length) {
                    print("VDEC_REQUEST_FAIL p-bad-capture\n"); return 1;
                }
                for (unsigned y = 0; y < V_HEIGHT; y++)
                    for (unsigned x = 0; x < V_WIDTH; x++)
                        if (q[y * stride + x] != vector_p_reference[y * V_WIDTH + x]) {
                            print("VDEC_REQUEST_FAIL p-luma-mismatch\n"); return 1;
                        }
                for (unsigned y = 0; y < V_HEIGHT / 2; y++)
                    for (unsigned x = 0; x < V_WIDTH / 2; x++)
                        if (q[stride * height + y * stride + 2 * x] != vector_p_reference[V_WIDTH * V_HEIGHT + y * (V_WIDTH / 2) + x] ||
                            q[stride * height + y * stride + 2 * x + 1] != vector_p_reference[V_WIDTH * V_HEIGHT * 5 / 4 + y * (V_WIDTH / 2) + x]) {
                            print("VDEC_REQUEST_FAIL p-chroma-mismatch\n"); return 1;
                        }
                print("VDEC_P_DECODE_MATCH\n");
                if (V_HAS_P2) {
                    srcplane = (struct v4l2_plane){0};
                    if (!check("p2-dequeue-output", call(29, video, VIDIOC_DQBUF, (long)&src, 0, 0, 0))) return 1;
                    if (!check("p2-reinit", call(29, request, MEDIA_REQUEST_IOC_REINIT, 0, 0, 0, 0))) return 1;
                    if (sizeof(vector_p2) > srcplane.length) { print("VDEC_REQUEST_FAIL p2-output-too-small\n"); return 1; }
                    for (u64 i = 0; i < sizeof(vector_p2); i++) ((unsigned char *)mapped)[i] = vector_p2[i];
                    decode.nal_ref_idc = V_P2_REF;
                    decode.frame_num = 2;
                    decode.top_field_order_cnt = 4;
                    decode.bottom_field_order_cnt = 4;
                    decode.dpb[0].reference_ts = 2000000000ULL;
                    decode.dpb[0].frame_num = 1;
                    decode.dpb[0].pic_num = 1;
                    decode.dpb[0].top_field_order_cnt = 2;
                    decode.dpb[0].bottom_field_order_cnt = 2;
                    if (!check("p2-set-controls", call(29, video, VIDIOC_S_EXT_CTRLS, (long)&ext, 0, 0, 0))) return 1;
                    dst = (struct v4l2_buffer){ .index = 0, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                                .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &dstplane };
                    dstplane = (struct v4l2_plane){0};
                    if (!check("p2-queue-capture", call(29, video, VIDIOC_QBUF, (long)&dst, 0, 0, 0))) return 1;
                    src = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                               .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &srcplane,
                                               .flags = V4L2_BUF_FLAG_REQUEST_FD, .request_fd = request };
                    src.timestamp.tv_sec = 3;
                    srcplane = (struct v4l2_plane){ .bytesused = sizeof(vector_p2) };
                    if (!check("p2-queue-output", call(29, video, VIDIOC_QBUF, (long)&src, 0, 0, 0))) return 1;
                    if (!check("p2-request-queue", call(29, request, MEDIA_REQUEST_IOC_QUEUE, 0, 0, 0, 0))) return 1;
                    pollfd.revents = 0;
                    timeout = (struct timespec){ .tv_sec = 10 };
                    ret = call(73, (long)&pollfd, 1, (long)&timeout, 0, 0, 0);
                    if (!check("p2-poll", ret) || !ret) { print("VDEC_P2_TIMEOUT\n"); return 1; }
                    dstplane = (struct v4l2_plane){0};
                    if (!check("p2-dequeue", call(29, video, VIDIOC_DQBUF, (long)&dst, 0, 0, 0))) return 1;
                    if (dst.flags & V4L2_BUF_FLAG_ERROR) { print("VDEC_REQUEST_FAIL p2-bad-capture\n"); return 1; }
                    for (unsigned y = 0; y < V_HEIGHT; y++)
                        for (unsigned x = 0; x < V_WIDTH; x++)
                            if (p[y * stride + x] != vector_p2_reference[y * V_WIDTH + x]) {
                                print("VDEC_REQUEST_FAIL p2-luma-mismatch\n"); return 1;
                            }
                    for (unsigned y = 0; y < V_HEIGHT / 2; y++)
                        for (unsigned x = 0; x < V_WIDTH / 2; x++)
                            if (p[stride * height + y * stride + 2 * x] != vector_p2_reference[V_WIDTH * V_HEIGHT + y * (V_WIDTH / 2) + x] ||
                                p[stride * height + y * stride + 2 * x + 1] != vector_p2_reference[V_WIDTH * V_HEIGHT * 5 / 4 + y * (V_WIDTH / 2) + x]) {
                                print("VDEC_REQUEST_FAIL p2-chroma-mismatch\n"); return 1;
                            }
                    print("VDEC_P2_DECODE_MATCH\n");
                    for (unsigned n = 3; n < V_SEQ_COUNT; n++) {
                        unsigned which = n - 3;
                        srcplane = (struct v4l2_plane){0};
                        if (!check("seq-dequeue-output", call(29, video, VIDIOC_DQBUF, (long)&src, 0, 0, 0))) return 1;
                        if (!check("seq-reinit", call(29, request, MEDIA_REQUEST_IOC_REINIT, 0, 0, 0, 0))) return 1;
                        if (vector_seq_lengths[which] > srcplane.length) { print("VDEC_REQUEST_FAIL seq-output-size\n"); return 1; }
                        for (unsigned i = 0; i < vector_seq_lengths[which]; i++)
                            ((unsigned char *)mapped)[i] = vector_sequence[vector_seq_offsets[which] + i];
                        decode.nal_ref_idc = vector_seq_refs[which];
                        decode.frame_num = n % 16;
                        decode.top_field_order_cnt = decode.bottom_field_order_cnt = 2 * n;
                        decode.dpb[0].reference_ts = (u64)n * 1000000000ULL;
                        decode.dpb[0].frame_num = (n - 1) % 16;
                        decode.dpb[0].pic_num = (n - 1) % 16;
                        decode.dpb[0].top_field_order_cnt = decode.dpb[0].bottom_field_order_cnt = 2 * (n - 1);
                        if (!check("seq-controls", call(29, video, VIDIOC_S_EXT_CTRLS, (long)&ext, 0, 0, 0))) return 1;
                        dst = (struct v4l2_buffer){ .index = n % 2, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                                    .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &dstplane };
                        dstplane = (struct v4l2_plane){0};
                        if (!check("seq-queue-capture", call(29, video, VIDIOC_QBUF, (long)&dst, 0, 0, 0))) return 1;
                        src = (struct v4l2_buffer){ .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
                                                   .memory = V4L2_MEMORY_MMAP, .length = 1, .m.planes = &srcplane,
                                                   .flags = V4L2_BUF_FLAG_REQUEST_FD, .request_fd = request };
                        src.timestamp.tv_sec = n + 1;
                        srcplane = (struct v4l2_plane){ .bytesused = vector_seq_lengths[which] };
                        if (!check("seq-queue-output", call(29, video, VIDIOC_QBUF, (long)&src, 0, 0, 0))) return 1;
                        if (!check("seq-request", call(29, request, MEDIA_REQUEST_IOC_QUEUE, 0, 0, 0, 0))) return 1;
                        pollfd.revents = 0;
                        timeout = (struct timespec){ .tv_sec = 30 };
                        ret = call(73, (long)&pollfd, 1, (long)&timeout, 0, 0, 0);
                        if (!check("seq-poll", ret) || !ret) { print("VDEC_SEQUENCE_TIMEOUT\n"); return 1; }
                        dstplane = (struct v4l2_plane){0};
                        if (!check("seq-dequeue", call(29, video, VIDIOC_DQBUF, (long)&dst, 0, 0, 0))) return 1;
                        if ((dst.flags & V4L2_BUF_FLAG_ERROR) ||
                            frame_crc((unsigned char *)(n % 2 ? pcapture : capture), stride, height) != vector_seq_crcs[n]) {
                            print("VDEC_REQUEST_FAIL seq-frame "); hex(n); return 1;
                        }
                    }
                    if (V_SEQ_COUNT) {
                        if (!check("sequence-clock-end", call(113, 1, (long)&sequence_end, 0, 0, 0, 0))) return 1;
                        u64 elapsed = (sequence_end.tv_sec - sequence_start.tv_sec) * 1000000000ULL +
                                      sequence_end.tv_nsec - sequence_start.tv_nsec;
                        print("VDEC_SEQUENCE_ELAPSED_MS "); decimal(elapsed / 1000000); print("\n");
                        print("VDEC_SEQUENCE_MATCH\n");
                    }
                }
            } else {
                if (!(dst.flags & V4L2_BUF_FLAG_ERROR)) { print("VDEC_REQUEST_FAIL p-expected-error\n"); return 1; }
                print("VDEC_P_EXPECTED_ERROR\n");
            }
        }
    } else {
        if (!(dst.flags & V4L2_BUF_FLAG_ERROR)) {
            print("VDEC_REQUEST_FAIL expected-error-buffer\n"); return 1;
        }
        print("VDEC_REQUEST_EXPECTED_ERROR\n");
    }
    return 0;
}

__asm__(".global _start\n_start:\nbl start_c\n");
__attribute__((used, noreturn)) void start_c(void)
{
    int rc = run();
    call(93, rc, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
