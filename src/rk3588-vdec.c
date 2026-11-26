/* Experimental VDPU381 front end: restricted host-backed H.264 decode. */
#include "qemu/osdep.h"
#include "qemu/module.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "trace/trace-hw_arm.h"
#include "qapi/error.h"
#include "rk3588-vdec.h"
#include <dlfcn.h>

#if __has_include(<libavcodec/avcodec.h>) && __has_include(<libavutil/frame.h>)
#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>
#define RK_VDEC_HAVE_AV_HEADERS 1
#else
#define RK_VDEC_HAVE_AV_HEADERS 0
#endif

#define TYPE_RK3588_VDEC "rk3588-vdec-experimental"
OBJECT_DECLARE_SIMPLE_TYPE(RkVdecState, RK3588_VDEC)

struct RkVdecState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    RkIommuState *iommu;
    uint32_t regs[0x500 / 4];
#if RK_VDEC_HAVE_AV_HEADERS
    AVCodecContext *session_ctx;
    AVFrame *session_last;
    void *session_lib, *session_util;
    void (*session_free_ctx)(AVCodecContext **);
    void (*session_free_frame)(AVFrame **);
    unsigned session_count;
#endif
    uint32_t idr_dst, idr_pps;
    uint8_t idr_params[48];
};

#if RK_VDEC_HAVE_AV_HEADERS
static void rk_vdec_session_clear(RkVdecState *s)
{
    if (s->session_last) s->session_free_frame(&s->session_last);
    if (s->session_ctx) s->session_free_ctx(&s->session_ctx);
    if (s->session_util) dlclose(s->session_util);
    if (s->session_lib) dlclose(s->session_lib);
    s->session_lib = s->session_util = NULL;
    s->session_count = 0;
}

/* Small bounded RBSP reader, for checking the narrow two-picture subset. */
typedef struct RkVdecBits {
    uint8_t data[128];
    unsigned size, pos;
    bool error;
} RkVdecBits;

static void rk_vdec_bits_init(RkVdecBits *b, const uint8_t *p, size_t len)
{
    unsigned zeros = 0;
    memset(b, 0, sizeof(*b));
    for (size_t i = 0; i < len && b->size < sizeof(b->data); i++) {
        if (zeros >= 2 && p[i] == 3) {
            zeros = 0;
            continue;
        }
        b->data[b->size++] = p[i];
        zeros = p[i] == 0 ? zeros + 1 : 0;
    }
}

static unsigned rk_vdec_bits_get(RkVdecBits *b, unsigned n)
{
    unsigned v = 0;
    if (n > 24 || b->pos + n > b->size * 8) {
        b->error = true;
        return 0;
    }
    while (n--) {
        v = (v << 1) | ((b->data[b->pos / 8] >> (7 - b->pos % 8)) & 1);
        b->pos++;
    }
    return v;
}

static unsigned rk_vdec_bits_ue(RkVdecBits *b)
{
    unsigned n = 0;
    while (!b->error && !rk_vdec_bits_get(b, 1)) {
        if (++n > 20) {
            b->error = true;
            return 0;
        }
    }
    return ((1u << n) - 1) + rk_vdec_bits_get(b, n);
}

static bool rk_vdec_simple_sps(const uint8_t *p, size_t n)
{
    RkVdecBits b;
    unsigned profile, id, framebits, poc, refs, gaps, width, height, progressive;
    rk_vdec_bits_init(&b, p, n);
    profile = rk_vdec_bits_get(&b, 8);
    rk_vdec_bits_get(&b, 16); /* constraints and level */
    id = rk_vdec_bits_ue(&b);
    framebits = rk_vdec_bits_ue(&b);
    poc = rk_vdec_bits_ue(&b);
    if (poc != 2) return false;
    refs = rk_vdec_bits_ue(&b);
    gaps = rk_vdec_bits_get(&b, 1);
    width = rk_vdec_bits_ue(&b);
    height = rk_vdec_bits_ue(&b);
    progressive = rk_vdec_bits_get(&b, 1);
    return !b.error && profile == 66 && id == 0 && framebits == 0 &&
           refs == 1 && !gaps && width == 119 && height == 67 && progressive;
}

static bool rk_vdec_simple_p(const uint8_t *p, size_t n, unsigned expected_frame)
{
    RkVdecBits b;
    unsigned first, type, pps, frame, override, modify, adaptive;
    rk_vdec_bits_init(&b, p, n);
    first = rk_vdec_bits_ue(&b);
    type = rk_vdec_bits_ue(&b);
    pps = rk_vdec_bits_ue(&b);
    frame = rk_vdec_bits_get(&b, 4);
    override = rk_vdec_bits_get(&b, 1);
    if (override) return false;
    modify = rk_vdec_bits_get(&b, 1);
    if (modify) return false;
    adaptive = rk_vdec_bits_get(&b, 1);
    return !b.error && first == 0 && type % 5 == 0 && pps == 0 &&
           frame == expected_frame && !adaptive;
}

static bool rk_vdec_simple_pps(const uint8_t *p, size_t n)
{
    RkVdecBits b;
    unsigned id, sps, entropy, bottom, groups, l0, l1, weighted, bipred, redundant;
    rk_vdec_bits_init(&b, p, n);
    id = rk_vdec_bits_ue(&b);
    sps = rk_vdec_bits_ue(&b);
    entropy = rk_vdec_bits_get(&b, 1);
    bottom = rk_vdec_bits_get(&b, 1);
    groups = rk_vdec_bits_ue(&b);
    if (groups) return false;
    l0 = rk_vdec_bits_ue(&b);
    l1 = rk_vdec_bits_ue(&b);
    weighted = rk_vdec_bits_get(&b, 1);
    bipred = rk_vdec_bits_get(&b, 2);
    rk_vdec_bits_ue(&b); /* signed pic_init_qp_minus26 */
    rk_vdec_bits_ue(&b); /* signed pic_init_qs_minus26 */
    rk_vdec_bits_ue(&b); /* signed chroma_qp_index_offset */
    rk_vdec_bits_get(&b, 2); /* deblocking and constrained intra */
    redundant = rk_vdec_bits_get(&b, 1);
    return !b.error && !id && !sps && !entropy && !bottom && !l0 && !l1 &&
           !weighted && !bipred && !redundant;
}

static bool rk_vdec_reference_matches(RkVdecState *s, AVFrame *f,
                                      uint32_t addr, unsigned stride,
                                      unsigned ysize)
{
    uint8_t row[1920];
    if (f->width != 1920 || f->height != 1080 || stride < f->width ||
        ysize / stride < f->height || f->format != AV_PIX_FMT_YUV420P ||
        (uint64_t)addr + ysize + (uint64_t)stride * 539 + f->width > (UINT64_C(1) << 32)) {
        return false;
    }
    for (unsigned y = 0; y < f->height; y++) {
        if (rk3588_iommu_dma_read(s->iommu, addr + y * stride, row, f->width) != MEMTX_OK ||
            memcmp(row, f->data[0] + y * f->linesize[0], f->width)) return false;
    }
    for (unsigned y = 0; y < f->height / 2; y++) {
        if (rk3588_iommu_dma_read(s->iommu, addr + ysize + y * stride, row, f->width) != MEMTX_OK) return false;
        for (unsigned x = 0; x < f->width / 2; x++) {
            if (row[2 * x] != f->data[1][y * f->linesize[1] + x] ||
                row[2 * x + 1] != f->data[2][y * f->linesize[2] + x]) return false;
        }
    }
    return true;
}
#endif

/* Restricted baseline POC-2, single-reference sequence. */
static bool rk_vdec_decode_keyframe(RkVdecState *s, uint32_t addr, size_t len)
{
#if RK_VDEC_HAVE_AV_HEADERS
    void *lib = NULL, *util = NULL;
    AVCodecContext *ctx = NULL;
    AVPacket *packet = NULL;
    AVFrame *frame = NULL;
    uint8_t *input = NULL;
    uint8_t row[1920];
    uint32_t dst = s->regs[0x208 / 4];
    unsigned stride = (s->regs[0x48 / 4] & 0xffff) * 16;
    unsigned uvstride = (s->regs[0x4c / 4] & 0xffff) * 16;
    unsigned ysize = (s->regs[0x50 / 4] & 0x0fffffff) * 16;
    unsigned sps = 0, pps = 0, idr = 0, pcount = 0;
    bool other_vcl = false, ok = false, simple_sps = false, simple_pps = false;
    bool simple_p = false, is_p = false, borrowed_ctx = false;
    const AVCodec *(*find_decoder)(enum AVCodecID);
    AVCodecContext *(*alloc_context)(const AVCodec *);
    int (*open_decoder)(AVCodecContext *, const AVCodec *, AVDictionary **);
    int (*send_packet)(AVCodecContext *, const AVPacket *);
    int (*receive_frame)(AVCodecContext *, AVFrame *);
    void (*free_context)(AVCodecContext **) = NULL;
    AVPacket *(*packet_alloc)(void);
    int (*new_packet)(AVPacket *, int);
    void (*packet_free)(AVPacket **) = NULL;
    AVFrame *(*frame_alloc)(void);
    AVFrame *(*frame_clone)(const AVFrame *);
    void (*frame_free)(AVFrame **) = NULL;
    const AVCodec *codec;

    if ((s->regs[0x24 / 4] & 0x3ff) != 1 || len < 8 || len > 1024 * 1024 ||
        stride < 64 || stride > 2048 || uvstride != stride ||
        !ysize || ysize % stride || ysize / stride > 1088) {
        goto out;
    }
    input = g_malloc(len);
    if (rk3588_iommu_dma_read(s->iommu, addr, input, len) != MEMTX_OK) {
        goto out;
    }
    /* The driver normally submits slice-only IDRs.  VDPU381's packed
     * parameter block has no crop rectangle or H.264 level, so do not guess
     * those for arbitrary streams.  This first translation is restricted to
     * the proven 1920x1080 baseline, one-reference POC-2 configuration.  The
     * generated SPS below specifies level 4 and crops 1088 coded lines to
     * 1080; the PPS selects CAVLC, default reference lists and deblocking. */
    if (len >= 4 && input[0] == 0 && input[1] == 0 && input[2] == 1 &&
        (input[3] & 31) == 5 && stride == 1920 && ysize == 1920 * 1088) {
        static const uint8_t hw_sps[] = { 0x20, 0x24, 0x00, 0x04, 0x01,
                                          0x1e, 0x10, 0x41, 0x07, 0x00 };
        static const uint8_t hw_pps[] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00 };
        static const uint8_t headers[] = {
            0, 0, 1, 0x67, 0x42, 0xc0, 0x28, 0xda, 0x01, 0xe0, 0x08, 0x9f, 0x95,
            0, 0, 1, 0x68, 0xce, 0x3c, 0x80,
        };
        uint8_t params[48];
        if (rk3588_iommu_dma_read(s->iommu, s->regs[0x284 / 4], params,
                                  sizeof(params)) == MEMTX_OK &&
            !memcmp(params, hw_sps, sizeof(hw_sps)) &&
            !memcmp(params + 32, hw_pps, sizeof(hw_pps))) {
            uint8_t *full = g_malloc(len + sizeof(headers));
            memcpy(full, headers, sizeof(headers));
            memcpy(full + sizeof(headers), input, len);
            g_free(input);
            input = full;
            len += sizeof(headers);
        }
    }
    for (size_t i = 0; i + 4 < len; i++) {
        if (input[i] == 0 && input[i + 1] == 0 && input[i + 2] == 1) {
            unsigned type = input[i + 3] & 31;
            sps += type == 7;
            pps += type == 8;
            idr += type == 5;
            pcount += type == 1;
            if (type == 7) simple_sps = rk_vdec_simple_sps(input + i + 4, len - i - 4);
            if (type == 8) simple_pps = rk_vdec_simple_pps(input + i + 4, len - i - 4);
            if (type == 1 && (input[i + 3] & 0x60))
                simple_p = s->session_ctx && s->session_count < 100000 &&
                    rk_vdec_simple_p(input + i + 4, len - i - 4, s->session_count % 16);
            other_vcl |= (type >= 2 && type <= 4) ||
                         (type >= 19 && type <= 21);
        }
    }
    is_p = !sps && !pps && !idr && pcount == 1 && simple_p && !other_vcl;
    if (is_p) {
        uint8_t params[sizeof(s->idr_params)];
        if (!s->session_ctx || !s->session_last || s->idr_pps != s->regs[0x284 / 4] ||
            s->idr_dst != s->regs[0x290 / 4] || dst == s->idr_dst ||
            s->regs[0x104 / 4] != 2 * s->session_count ||
            s->regs[0x108 / 4] != 2 * s->session_count ||
            s->regs[0x10c / 4] != 2 * (s->session_count - 1) ||
            s->regs[0x110 / 4] != 2 * (s->session_count - 1) ||
            (s->regs[0x18c / 4] & 0xff) != 0xe ||
            (s->regs[0x18c / 4] & 0xffffff00) ||
            s->regs[0x190 / 4] || s->regs[0x194 / 4] || s->regs[0x198 / 4] ||
            rk3588_iommu_dma_read(s->iommu, s->idr_pps, params, sizeof(params)) != MEMTX_OK ||
            memcmp(params, s->idr_params, sizeof(params))) goto out;
    } else if (sps != 1 || pps != 1 || idr != 1 || pcount || other_vcl) {
        goto out;
    }
    lib = dlopen("libavcodec.so." G_STRINGIFY(LIBAVCODEC_VERSION_MAJOR), RTLD_NOW | RTLD_LOCAL);
    util = dlopen("libavutil.so." G_STRINGIFY(LIBAVUTIL_VERSION_MAJOR), RTLD_NOW | RTLD_LOCAL);
    if (!lib || !util) {
        goto out;
    }
#define LOAD(handle, var, name) do { *(void **)(&(var)) = dlsym(handle, name); if (!(var)) goto out; } while (0)
    LOAD(lib, find_decoder, "avcodec_find_decoder");
    LOAD(lib, alloc_context, "avcodec_alloc_context3");
    LOAD(lib, open_decoder, "avcodec_open2");
    LOAD(lib, send_packet, "avcodec_send_packet");
    LOAD(lib, receive_frame, "avcodec_receive_frame");
    LOAD(lib, free_context, "avcodec_free_context");
    LOAD(lib, packet_alloc, "av_packet_alloc");
    LOAD(lib, new_packet, "av_new_packet");
    LOAD(lib, packet_free, "av_packet_free");
    LOAD(util, frame_alloc, "av_frame_alloc");
    LOAD(util, frame_clone, "av_frame_clone");
    LOAD(util, frame_free, "av_frame_free");
#undef LOAD
    if (is_p) {
        if (!rk_vdec_reference_matches(s, s->session_last, s->idr_dst, stride, ysize)) goto out;
        ctx = s->session_ctx;
        borrowed_ctx = true;
    } else {
        rk_vdec_session_clear(s);
        codec = find_decoder(AV_CODEC_ID_H264);
        if (!codec || !(ctx = alloc_context(codec))) goto out;
        ctx->max_pixels = 1920 * 1088;
        ctx->thread_count = 1;
        ctx->err_recognition = AV_EF_EXPLODE;
        if (open_decoder(ctx, codec, NULL) < 0) goto out;
    }
    if (!(packet = packet_alloc()) || !(frame = frame_alloc())) goto out;
    if (new_packet(packet, len) < 0) goto out;
    memcpy(packet->data, input, len);
    if (send_packet(ctx, packet) < 0 || receive_frame(ctx, frame) < 0 ||
        !((frame->width == 64 && frame->height == 64) ||
          (frame->width == 1920 && frame->height == 1080)) ||
        stride < frame->width || ysize / stride < frame->height ||
        (uint64_t)dst + ysize + (uint64_t)stride * (frame->height / 2 - 1) +
            frame->width > (UINT64_C(1) << 32) ||
        frame->format != AV_PIX_FMT_YUV420P || frame->decode_error_flags ||
        (is_p && frame->pict_type != AV_PICTURE_TYPE_P)) {
        goto out;
    }
    for (unsigned y = 0; y < frame->height; y++) {
        if (rk3588_iommu_dma_write(s->iommu, dst + y * stride,
                                   frame->data[0] + y * frame->linesize[0], frame->width) != MEMTX_OK) {
            goto out;
        }
    }
    for (unsigned y = 0; y < frame->height / 2; y++) {
        for (unsigned x = 0; x < frame->width / 2; x++) {
            row[2 * x] = frame->data[1][y * frame->linesize[1] + x];
            row[2 * x + 1] = frame->data[2][y * frame->linesize[2] + x];
        }
        if (rk3588_iommu_dma_write(s->iommu, dst + ysize + y * stride, row, frame->width) != MEMTX_OK) {
            goto out;
        }
    }
    ok = true;
    if (is_p) {
        AVFrame *last = frame_clone(frame);
        if (!last) { ok = false; goto out; }
        s->session_free_frame(&s->session_last);
        s->session_last = last;
        s->session_count++;
        s->idr_dst = dst;
    }
    if (!is_p && simple_sps && simple_pps &&
        rk3588_iommu_dma_read(s->iommu, s->regs[0x284 / 4], s->idr_params,
                              sizeof(s->idr_params)) == MEMTX_OK) {
        s->session_last = frame_clone(frame);
        if (s->session_last) {
            s->session_ctx = ctx;
            s->session_lib = lib;
            s->session_util = util;
            s->session_free_ctx = free_context;
            s->session_free_frame = frame_free;
            s->session_count = 1;
            s->idr_dst = dst;
            s->idr_pps = s->regs[0x284 / 4];
            ctx = NULL;
            lib = util = NULL;
        }
    }
out:
    if (!ok) {
        rk_vdec_session_clear(s);
    }
    if (frame) frame_free(&frame);
    if (packet) packet_free(&packet);
    if (ctx && !borrowed_ctx) free_context(&ctx);
    if (util) dlclose(util);
    if (lib) dlclose(lib);
    g_free(input);
    return ok;
#else
    return false;
#endif
}

static uint64_t rk_vdec_read(void *opaque, hwaddr off, unsigned size)
{
    RkVdecState *s = opaque;
    return s->regs[off / 4];
}

static void rk_vdec_write(void *opaque, hwaddr off, uint64_t value,
                          unsigned size)
{
    RkVdecState *s = opaque;

    s->regs[off / 4] = value;
    if (off == 0x380) {
        /* Linux acknowledges by writing zero. */
        if (!value) {
            qemu_set_irq(s->irq, 0);
        }
    } else if (off == 0x28 && (value & 1)) {
        uint32_t addr = s->regs[0x200 / 4]; /* reg128: rlc_base */
        size_t len = MIN(s->regs[0x40 / 4], 16u); /* reg16: stream length */
        uint8_t data[16] = { 0 };
        MemTxResult result = len ? rk3588_iommu_dma_read(s->iommu, addr,
                                                        data, len) : MEMTX_ERROR;
        uint32_t sum = 0;

        if (result == MEMTX_OK) {
            for (size_t i = 0; i < len; i++) {
                sum += data[i];
            }
        }
        trace_rk3588_vdec_dma_probe(addr, len, result, sum);
        trace_rk3588_vdec_reference(s->regs[0x208 / 4], s->regs[0x290 / 4],
                                    s->regs[0x18c / 4] & 0xff);
        if (trace_event_get_state(TRACE_RK3588_VDEC_JOB_DATA)) {
            uint8_t prefix[32] = { 0 }, params[48] = { 0 };
            char prefix_hex[65], params_hex[97];
            size_t prefix_len = MIN(s->regs[0x40 / 4], sizeof(prefix));
            uint32_t pps_addr = s->regs[0x284 / 4];
            MemTxResult ir = prefix_len ? rk3588_iommu_dma_read(s->iommu, addr, prefix, prefix_len) : MEMTX_ERROR;
            MemTxResult pr = rk3588_iommu_dma_read(s->iommu, pps_addr, params, sizeof(params));
            for (size_t i = 0; i < sizeof(prefix); i++) {
                snprintf(prefix_hex + 2 * i, 3, "%02x", prefix[i]);
            }
            for (size_t i = 0; i < sizeof(params); i++) {
                snprintf(params_hex + 2 * i, 3, "%02x", params[i]);
            }
            trace_rk3588_vdec_job_data(s->regs[0x40 / 4], pps_addr, ir, pr, prefix_hex, params_hex);
        }
        bool decoded = rk_vdec_decode_keyframe(s, addr, s->regs[0x40 / 4]);
        trace_rk3588_vdec_result(decoded);
        /* Unsupported jobs and failed DMA are errors, never fake success. */
        s->regs[0x28 / 4] = 0;
        s->regs[0x380 / 4] = decoded ? 1u << 2 : 1u << 4;
        if (!(s->regs[0x2c / 4] & (1u << 4))) {
            qemu_set_irq(s->irq, 1);
        }
    }
}

static const MemoryRegionOps rk_vdec_ops = {
    .read = rk_vdec_read,
    .write = rk_vdec_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void rk_vdec_reset(DeviceState *dev)
{
    RkVdecState *s = RK3588_VDEC(dev);
#if RK_VDEC_HAVE_AV_HEADERS
    rk_vdec_session_clear(s);
#endif
    memset(s->regs, 0, sizeof(s->regs));
    qemu_set_irq(s->irq, 0);
}

static void rk_vdec_init(Object *obj)
{
    RkVdecState *s = RK3588_VDEC(obj);
    memory_region_init_io(&s->iomem, obj, &rk_vdec_ops, s,
                          "rk3588.vdec0", sizeof(s->regs));
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void rk_vdec_finalize(Object *obj)
{
    RkVdecState *s = RK3588_VDEC(obj);
#if RK_VDEC_HAVE_AV_HEADERS
    rk_vdec_session_clear(s);
#endif
}

static void rk_vdec_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_legacy_reset(DEVICE_CLASS(oc), rk_vdec_reset);
}

static const TypeInfo rk_vdec_type = {
    .name = TYPE_RK3588_VDEC,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(RkVdecState),
    .instance_init = rk_vdec_init,
    .instance_finalize = rk_vdec_finalize,
    .class_init = rk_vdec_class_init,
};

static void rk_vdec_register(void)
{
    type_register_static(&rk_vdec_type);
}
type_init(rk_vdec_register)

void rk3588_create_vdec0(DeviceState *gic, RkIommuState *iommu)
{
    DeviceState *dev = qdev_new(TYPE_RK3588_VDEC);
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);

    RK3588_VDEC(dev)->iommu = iommu;
    sysbus_realize_and_unref(sbd, &error_fatal);
    memory_region_add_subregion_overlap(get_system_memory(), 0xfdc38100,
                                        sysbus_mmio_get_region(sbd, 0), 1);
    sysbus_connect_irq(sbd, 0, qdev_get_gpio_in(gic, 95));
}
