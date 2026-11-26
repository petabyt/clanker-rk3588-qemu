/* Invoke USB routines in the unchanged RK3588 ROM dump, not its full boot. */
#include <stdint.h>

int main(void)
{
    uint64_t (*init)(void) = (void *)(uintptr_t)0xffff424c;
    void (*poll)(void) = (void *)(uintptr_t)0xffff3dfc;
    volatile uint32_t *result = (void *)(uintptr_t)0xff0c0000;

    *result = 0x12345678;
    result[1] = init();
    *result = 0x87654321;
    for (;;) {
        poll();
        result[2]++;
    }
}
