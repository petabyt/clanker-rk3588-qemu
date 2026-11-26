/* Minimal DWC3 gadget fixture for QEMU regression, not production firmware. */
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define BASE 0xfc000000UL
#define R(off) REG(BASE + (off))

struct trb { uint32_t lo, hi, size, control; };
static volatile uint32_t events[64] __attribute__((aligned(64)));
static volatile struct trb trbs[4] __attribute__((aligned(16)));
static volatile uint8_t setup[8] __attribute__((aligned(16)));
static volatile uint8_t line_coding[7] __attribute__((aligned(16)));
static volatile uint8_t rx_buffer[64] __attribute__((aligned(16)));
static const uint8_t descriptor[18] = {
    18, 1, 0, 2, 0, 0, 0, 64, 0x78, 0x56, 0x34, 0x12, 0, 1, 0, 0, 0, 1
};
static const uint8_t message[] = "UART over OTG\n";
static const uint8_t configuration[] = {
    9, 2, 67, 0, 2, 1, 0, 0x80, 50,
    9, 4, 0, 0, 1, 2, 2, 1, 0,
    5, 0x24, 0, 0x10, 0x01,
    5, 0x24, 1, 0, 1,
    4, 0x24, 2, 2,
    5, 0x24, 6, 0, 1,
    7, 5, 0x82, 3, 16, 0, 16,
    9, 4, 1, 0, 2, 0x0a, 0, 0, 0,
    7, 5, 0x01, 2, 0, 2, 0,
    7, 5, 0x81, 2, 0, 2, 0
};
static unsigned event_pos, stage;

static void command(unsigned ep, uint32_t cmd, uint32_t p0, uint32_t p1)
{
    unsigned base = 0xc800 + ep * 16;
    R(base) = 0;
    R(base + 4) = p1;
    R(base + 8) = p0;
    R(base + 12) = cmd | 0x400;
    while (R(base + 12) & 0x400) { }
}

static void configure(unsigned ep, unsigned type, unsigned mps)
{
    command(ep, 1, (type << 1) | (mps << 3), (ep << 25) | 0x500);
    command(ep, 2, 1, 0);
    R(0xc720) |= 1U << ep;
}

static void start(unsigned ep, uintptr_t buf, uint32_t size, unsigned type)
{
    volatile struct trb *t = &trbs[ep];
    t->lo = buf;
    t->hi = (uint64_t)buf >> 32;
    t->size = size;
    t->control = (type << 4) | 0x803;
    __asm__ volatile("dsb sy" ::: "memory");
    command(ep, 6, (uint64_t)(uintptr_t)t >> 32, (uintptr_t)t);
}

static void setup_next(void)
{
    stage = 0;
    start(0, (uintptr_t)setup, sizeof(setup), 2);
    REG(0xff0c0000)++;
}

int main(void)
{
    R(0xc704) = 1U << 30;
    R(0xc400) = (uintptr_t)events;
    R(0xc404) = (uint64_t)(uintptr_t)events >> 32;
    R(0xc408) = sizeof(events);
    R(0xc708) = 6;
    command(0, 9, 0, 0);
    configure(0, 0, 64);
    configure(1, 0, 64);
    setup_next();
    R(0xc704) = 1U << 31;
    for (;;) {
        if (!(R(0xc40c) & 0xfffc)) {
            continue;
        }
        uint32_t event = events[event_pos++ & 63];
        R(0xc40c) = 4;
        if ((event & 1) || ((event >> 6) & 15) != 1) {
            continue;
        }
        unsigned ep = (event >> 1) & 31;
        if (ep == 0 && stage == 0) {
            if (setup[1] == 5) {
                stage = 1;
                start(1, 0, 0, 3);
            } else if (setup[1] == 6 && (setup[3] == 1 || setup[3] == 2)) {
                const uint8_t *desc = setup[3] == 1 ? descriptor : configuration;
                unsigned size = setup[3] == 1 ? sizeof(descriptor) : sizeof(configuration);
                unsigned requested = setup[6] | ((unsigned)setup[7] << 8);
                if (requested < size) size = requested;
                stage = 2;
                start(1, (uintptr_t)desc, size, 5);
            } else if (setup[1] == 9) {
                configure(2, 2, 512);
                configure(3, 2, 512);
                configure(5, 3, 16);
                start(2, (uintptr_t)rx_buffer, sizeof(rx_buffer), 1);
                start(3, (uintptr_t)message, sizeof(message) - 1, 1);
                stage = 1;
                start(1, 0, 0, 3);
            } else if (setup[0] == 0x21 && setup[1] == 0x20 && setup[6] == 7) {
                stage = 4;
                start(0, (uintptr_t)line_coding, sizeof(line_coding), 5);
            } else if (setup[0] == 0x21 && setup[1] == 0x22) {
                stage = 1;
                start(1, 0, 0, 3);
            } else {
                command(0, 4, 0, 0);
            }
        } else if (ep == 0 && stage == 4) {
            REG(0xff0c0008) = line_coding[0] | (line_coding[1] << 8) |
                              (line_coding[2] << 16) | (line_coding[3] << 24);
            stage = 1;
            start(1, 0, 0, 4);
        } else if (ep == 1 && stage == 2) {
            stage = 3;
            start(0, 0, 0, 4);
        } else if ((ep == 1 && stage == 1) || (ep == 0 && stage == 3)) {
            setup_next();
        } else if (ep == 2) {
            REG(0xff0c0004) = rx_buffer[0] | (rx_buffer[1] << 8) |
                              (rx_buffer[2] << 16) | (rx_buffer[3] << 24);
            start(2, (uintptr_t)rx_buffer, sizeof(rx_buffer), 1);
        }
    }
}
