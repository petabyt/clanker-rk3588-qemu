/* Minimal freestanding AArch64 V4L2 discovery tool for the initramfs.
 * The structures and ioctl numbers below are the Linux UAPI ABI; no libc or
 * kernel-internal structures are involved. This submits no decode requests.
 */
typedef unsigned int u32;
typedef unsigned long u64;
typedef unsigned char u8;

struct v4l2_capability {
    u8 driver[16], card[32], bus_info[32];
    u32 version, capabilities, device_caps, reserved[3];
};
struct v4l2_fmtdesc {
    u32 index, type, flags;
    u8 description[32];
    u32 pixelformat, mbus_code, reserved[3];
};
struct media_device_info {
    char driver[16], model[32], serial[40], bus_info[32];
    u32 media_version, hw_revision, driver_version, reserved[31];
};
struct v4l2_requestbuffers {
    u32 count, type, memory, capabilities;
    u8 flags, reserved[3];
};
_Static_assert(sizeof(struct v4l2_capability) == 104, "V4L2 capability ABI");
_Static_assert(sizeof(struct v4l2_fmtdesc) == 64, "V4L2 format ABI");
_Static_assert(sizeof(struct media_device_info) == 256, "media device ABI");
_Static_assert(sizeof(struct v4l2_requestbuffers) == 20, "requestbuffers ABI");

#define VIDIOC_QUERYCAP 0x80685600u
#define VIDIOC_ENUM_FMT 0xc0405602u
#define VIDIOC_REQBUFS 0xc0145608u
#define V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE 9
#define V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE 10
#define H264_SLICE 0x34363253u /* S264 */
#define MEDIA_IOC_DEVICE_INFO 0xc1007c00u
#define MEDIA_IOC_REQUEST_ALLOC 0x80047c05u
#define MEDIA_REQUEST_IOC_REINIT 0x7c81u
#define MEDIA_REQUEST_IOC_QUEUE 0x7c80u
#define V4L2_BUF_CAP_SUPPORTS_REQUESTS 8
#define EINVAL 22
#define ENOENT 2

void *memset(void *dst, int c, u64 n)
{
    u8 *p = dst;
    while (n--) {
        *p++ = c;
    }
    return dst;
}

static long syscall4(long nr, long a, long b, long c, long d)
{
    register long x0 __asm__("x0") = a;
    register long x1 __asm__("x1") = b;
    register long x2 __asm__("x2") = c;
    register long x3 __asm__("x3") = d;
    register long x8 __asm__("x8") = nr;

    __asm__ volatile("svc 0" : "+r" (x0) : "r" (x1), "r" (x2),
                     "r" (x3), "r" (x8) : "memory", "cc");
    return x0;
}

static void print(const char *s)
{
    u64 n = 0;
    while (s[n]) {
        n++;
    }
    syscall4(64, 1, (long)s, n, 0); /* write */
}

static void hex(u32 n)
{
    char s[] = "0x00000000\n";
    const char *digits = "0123456789abcdef";
    for (int i = 0; i < 8; i++) {
        s[9 - i] = digits[n & 15];
        n >>= 4;
    }
    print(s);
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int probe_media(const struct v4l2_capability *cap, int video)
{
    for (int i = 0; i < 64; i++) {
        char path[] = "/dev/media00";
        struct media_device_info info = { 0 };
        int request = -1;
        long fd, ret;

        if (i < 10) {
            path[10] = '0' + i;
            path[11] = 0;
        } else {
            path[10] = '0' + i / 10;
            path[11] = '0' + i % 10;
        }
        fd = syscall4(56, -100, (long)path, 2, 0);
        if (fd < 0) {
            continue;
        }
        ret = syscall4(29, fd, MEDIA_IOC_DEVICE_INFO, (long)&info, 0);
        info.driver[15] = info.model[31] = info.bus_info[31] = 0;
        if (ret < 0 || !same(info.driver, (const char *)cap->driver) ||
            !same(info.bus_info, (const char *)cap->bus_info)) {
            syscall4(57, fd, 0, 0, 0);
            continue;
        }
        print("VDEC_MEDIA_DEVICE "); print(path); print("\n");
        ret = syscall4(29, fd, MEDIA_IOC_REQUEST_ALLOC, (long)&request, 0);
        if (ret < 0) {
            print("VDEC_REQUEST_ALLOC_ERROR "); hex(-ret);
            syscall4(57, fd, 0, 0, 0);
            return 1;
        }
        /* The stock rkvdec validator must reject a request without a buffer. */
        ret = syscall4(29, request, MEDIA_REQUEST_IOC_QUEUE, 0, 0);
        if (ret != -ENOENT) {
            print("VDEC_EMPTY_REQUEST_UNEXPECTED "); hex(-ret);
            syscall4(57, request, 0, 0, 0);
            syscall4(57, fd, 0, 0, 0);
            return 1;
        }
        print("VDEC_EMPTY_REQUEST_REJECTED\n");
        ret = syscall4(29, request, MEDIA_REQUEST_IOC_REINIT, 0, 0);
        syscall4(57, request, 0, 0, 0);
        syscall4(57, fd, 0, 0, 0);
        if (ret < 0) {
            print("VDEC_REQUEST_REINIT_ERROR "); hex(-ret);
            return 1;
        }
        print("VDEC_REQUEST_ALLOC_REINIT_OK\n");
        for (u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
             type <= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; type++) {
            struct v4l2_requestbuffers buf = { .count = 1, .type = type,
                                               .memory = 1 /* MMAP */ };
            ret = syscall4(29, video, VIDIOC_REQBUFS, (long)&buf, 0);
            if (ret < 0 || !buf.count ||
                (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
                 !(buf.capabilities & V4L2_BUF_CAP_SUPPORTS_REQUESTS))) {
                print("VDEC_REQBUFS_ERROR ");
                hex(ret < 0 ? -ret : 0);
                return 1;
            }
            print(type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ?
                  "VDEC_OUTPUT_BUFFER_ALLOC_OK\n" :
                  "VDEC_CAPTURE_BUFFER_ALLOC_OK\n");
            buf.count = 0;
            ret = syscall4(29, video, VIDIOC_REQBUFS, (long)&buf, 0);
            if (ret < 0) {
                print("VDEC_REQBUFS_RELEASE_ERROR "); hex(-ret);
                return 1;
            }
        }
        return 0;
    }
    print("VDEC_MEDIA_DEVICE_NOT_FOUND\n");
    return 1;
}

static int probe(const char *path)
{
    struct v4l2_capability cap = { 0 };
    long fd = syscall4(56, -100, (long)path, 2, 0); /* openat, O_RDWR */
    int h264 = 0;

    print("VDEC_IOCTL_DEVICE ");
    print(path);
    print("\n");
    if (fd < 0) {
        print("VDEC_IOCTL_OPEN_ERROR ");
        hex(-fd);
        return 1;
    }
    long ret = syscall4(29, fd, VIDIOC_QUERYCAP, (long)&cap, 0);
    if (ret < 0) {
        print("VDEC_IOCTL_QUERYCAP_ERROR ");
        hex(-ret);
        return 1;
    }
    cap.driver[15] = cap.card[31] = cap.bus_info[31] = 0;
    print("VDEC_IOCTL_DRIVER "); print((char *)cap.driver); print("\n");
    print("VDEC_IOCTL_CARD "); print((char *)cap.card); print("\n");
    print("VDEC_IOCTL_BUS "); print((char *)cap.bus_info); print("\n");
    print("VDEC_IOCTL_CAPS "); hex(cap.device_caps);
    for (u32 type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
         type <= V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE; type++) {
        for (u32 i = 0; i < 128; i++) {
            struct v4l2_fmtdesc fmt = { .index = i, .type = type };
            char fourcc[5];

            ret = syscall4(29, fd, VIDIOC_ENUM_FMT, (long)&fmt, 0);
            if (ret == -EINVAL) {
                break;
            }
            if (ret < 0) {
                print("VDEC_IOCTL_ENUM_ERROR "); hex(-ret);
                return 1;
            }
            for (int j = 0; j < 4; j++) {
                fourcc[j] = (fmt.pixelformat >> (8 * j)) & 0xff;
            }
            fourcc[4] = 0;
            print(type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE ?
                  "VDEC_CAPTURE_FORMAT " : "VDEC_OUTPUT_FORMAT ");
            print(fourcc); print("\n");
            if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
                fmt.pixelformat == H264_SLICE) {
                h264 = 1;
            }
        }
    }
    print(h264 ? "VDEC_IOCTL_H264_SLICE_PRESENT\n" :
                 "VDEC_IOCTL_H264_SLICE_ABSENT\n");
    int result = h264 ? probe_media(&cap, fd) : 1;
    syscall4(57, fd, 0, 0, 0); /* close */
    return result;
}

/* Linux supplies argc/argv on the initial stack; avoid a C runtime. */
__asm__(".global _start\n"
        "_start:\n"
        "mov x0, sp\n"
        "b start_c\n");

__attribute__((used, noreturn)) void start_c(u64 *sp)
{
    int rc = sp[0] == 2 ? probe((const char *)sp[2]) : 2;
    syscall4(93, rc, 0, 0, 0); /* exit */
    __builtin_unreachable();
}
