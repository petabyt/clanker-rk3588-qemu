/* Experimental harness, NOT an emulation of the RK3568 BootROM contract.
 * Load at 0xfdcd1d00, DDR image at 0xfdcc1000. Keep the temporary stack in
 * PMU SRAM and pass a zeroed 32-byte block in system SRAM as the guessed x0.
 * The vendor binary copies 32 bytes from x0 before initialization.
 */
.text
.global _start
_start:
    ldr x0, =0xfdcd1d00
    mov sp, x0
    ldr x0, =0xfdcc0000
    mov x1, #8
    ldr x2, =0xfdcc1000
    blr x2
    cbnz w0, 2f
    adr x0, ok_message
    b 3f
2:  adr x0, error_message
3:  ldr x1, =0xfe660000
4:  ldrb w2, [x0], #1
    cbz w2, 5f
    str w2, [x1]
    b 4b
5:  wfe
    b 5b
ok_message:
    .asciz "\r\nDDR_PROBE_RETURN_OK\r\n"
error_message:
    .asciz "\r\nDDR_PROBE_RETURN_ERROR\r\n"
