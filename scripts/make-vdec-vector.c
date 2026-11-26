/* Generate a one-frame Annex B H.264 test vector and software reference.
 * Host-only utility: cc ... $(pkg-config --cflags --libs libavcodec libavutil)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

static void fail(const char *message, int rc)
{
    char error[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(rc, error, sizeof(error));
    fprintf(stderr, "%s: %s\n", message, error);
    exit(1);
}

static void write_file(const char *path, const uint8_t *data, size_t size)
{
    FILE *file = fopen(path, "wb");

    if (!file || fwrite(data, 1, size, file) != size || fclose(file)) {
        perror(path);
        exit(1);
    }
}

int main(int argc, char **argv)
{
    const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
    const AVCodec *decoder = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *enc, *dec;
    AVFrame *in, *out;
    AVPacket *pkt;
    uint8_t *yuv;
    int width = argc == 4 && !strcmp(argv[3], "1080p") ? 1920 : 64;
    int height = width == 1920 ? 1080 : 64;
    size_t yuv_size = (size_t)width * height * 3 / 2;
    int rc, offset = 0;

    if (argc != 3 && !(argc == 4 && !strcmp(argv[3], "1080p"))) {
        fprintf(stderr, "usage: %s frame.h264 reference.yuv [1080p]\n", argv[0]);
        return 2;
    }
    if (!encoder || !decoder) {
        fprintf(stderr, "libavcodec needs libx264 and an H.264 decoder\n");
        return 1;
    }
    enc = avcodec_alloc_context3(encoder);
    dec = avcodec_alloc_context3(decoder);
    in = av_frame_alloc();
    out = av_frame_alloc();
    pkt = av_packet_alloc();
    if (!enc || !dec || !in || !out || !pkt) {
        fprintf(stderr, "out of memory\n");
        return 1;
    }
    yuv = malloc(yuv_size);
    if (!yuv) return 1;
    enc->width = width;
    enc->height = height;
    enc->time_base = (AVRational){1, 25};
    enc->pix_fmt = AV_PIX_FMT_YUV420P;
    enc->gop_size = 1;
    enc->max_b_frames = 0;
    av_opt_set(enc->priv_data, "preset", "ultrafast", 0);
    av_opt_set(enc->priv_data, "tune", "zerolatency", 0);
    av_opt_set(enc->priv_data, "profile", "baseline", 0);
    av_opt_set(enc->priv_data, "x264-params",
               "threads=1:repeat-headers=1:annexb=1:scenecut=0", 0);
    if ((rc = avcodec_open2(enc, encoder, NULL)) < 0) {
        fail("open encoder", rc);
    }
    in->format = enc->pix_fmt;
    in->width = enc->width;
    in->height = enc->height;
    if ((rc = av_frame_get_buffer(in, 32)) < 0) {
        fail("allocate frame", rc);
    }
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            in->data[0][y * in->linesize[0] + x] = 16 + (x * 3 + y * 2) % 220;
        }
    }
    for (int plane = 1; plane < 3; plane++) {
        for (int y = 0; y < height / 2; y++) {
            memset(in->data[plane] + y * in->linesize[plane],
                   plane == 1 ? 96 : 160, width / 2);
        }
    }
    in->pts = 0;
    if ((rc = avcodec_send_frame(enc, in)) < 0 ||
        (rc = avcodec_receive_packet(enc, pkt)) < 0) {
        fail("encode frame", rc);
    }
    write_file(argv[1], pkt->data, pkt->size);
    if ((rc = avcodec_open2(dec, decoder, NULL)) < 0 ||
        (rc = avcodec_send_packet(dec, pkt)) < 0 ||
        (rc = avcodec_receive_frame(dec, out)) < 0) {
        fail("decode reference", rc);
    }
    if (out->width != width || out->height != height || out->format != AV_PIX_FMT_YUV420P) {
        fprintf(stderr, "unexpected reference dimensions or pixel format\n");
        return 1;
    }
    /* Reference is tightly packed planar YUV420, not the guest's NV12. */
    for (int plane = 0; plane < 3; plane++) {
        int pw = plane ? width / 2 : width;
        int ph = plane ? height / 2 : height;
        for (int y = 0; y < ph; y++) {
            memcpy(yuv + offset, out->data[plane] + y * out->linesize[plane], pw);
            offset += pw;
        }
    }
    write_file(argv[2], yuv, yuv_size);
    printf("H.264: %d bytes; reference: %zu bytes, %dx%d YUV420P\n",
           pkt->size, yuv_size, width, height);
    free(yuv);
    av_packet_free(&pkt);
    av_frame_free(&in);
    av_frame_free(&out);
    avcodec_free_context(&enc);
    avcodec_free_context(&dec);
    return 0;
}
