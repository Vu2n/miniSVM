// console.c - Tiny UEFI console output implementation.
#include "console.h"
#include "cpu.h"

#define COM1 0x3F8
#define COM2 0x2F8
// Initialize a 16550 UART (115200 8N1). Firmware may not have done it (QEMU's
// OVMF does; VMware may not), and an uninitialized UART can swallow output.
static void serial_init_port(unsigned short p) {
    __outbyte(p + 1, 0x00);   // disable interrupts
    __outbyte(p + 3, 0x80);   // DLAB on
    __outbyte(p + 0, 0x01);   // divisor low  = 115200
    __outbyte(p + 1, 0x00);   // divisor high
    __outbyte(p + 3, 0x03);   // 8 bits, no parity, 1 stop; DLAB off
    __outbyte(p + 2, 0xC7);   // enable + clear FIFOs
    __outbyte(p + 4, 0x0B);   // RTS/DSR set
}
static void serial_putc_port(unsigned short p, char c) {
    for (int i = 0; i < 100000; i++)
        if (__inbyte(p + 5) & 0x20) break;      // wait (bounded) for THR empty
    __outbyte(p, (unsigned char)c);
}
// Write to BOTH COM1 and COM2 - we don't know which one the VM's serial file is
// wired to, so send to both.
static void serial_putc(char c) {
    static BOOLEAN inited = FALSE;
    if (!inited) { serial_init_port(COM1); serial_init_port(COM2); inited = TRUE; }
    serial_putc_port(COM1, c);
    serial_putc_port(COM2, c);
}
void serial_puts(const char *s) { while (*s) serial_putc(*s++); }
void serial_ascii(const char *s, UINTN n) {
    for (UINTN i = 0; i < n; i++) {
        char c = s[i];
        if (c == 0) break;
        serial_putc((c >= 0x20 && c < 0x7F) ? c : '.');
    }
}

static EFI_SYSTEM_TABLE *gST;

void con_init(EFI_SYSTEM_TABLE *st) { gST = st; }

void print(const CHAR16 *s) {
    gST->ConOut->OutputString(gST->ConOut, (CHAR16 *)s);
    // Note: we do NOT mirror to serial here. QEMU's OVMF already routes the UEFI
    // console to COM1, so mirroring would double it. The important runtime logs
    // (once the OS is booting and the console is gone) use the raw serial dbg_*
    // helpers directly.
}

void print_hex(UINT64 v) {
    CHAR16 buf[19];
    buf[0] = L'0'; buf[1] = L'x';
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> ((15 - i) * 4)) & 0xF);
        buf[2 + i] = (CHAR16)(nib < 10 ? L'0' + nib : L'A' + (nib - 10));
    }
    buf[18] = 0;
    print(buf);
}

void print_line(const CHAR16 *label, UINT64 v) {
    print(label);
    print_hex(v);
    print(L"\r\n");
}

// Print up to n bytes of ASCII by widening each byte to UTF-16. Stops early at
// a NUL. Non-printable bytes are shown as '.'.
void print_ascii(const char *s, UINTN n) {
    CHAR16 buf[2];
    buf[1] = 0;
    for (UINTN i = 0; i < n; i++) {
        char c = s[i];
        if (c == 0) break;
        buf[0] = (CHAR16)((c >= 0x20 && c < 0x7F) ? c : '.');
        print(buf);
    }
}
