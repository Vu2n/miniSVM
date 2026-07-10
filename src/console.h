// console.h - Tiny UEFI console output, shared by main.c and the SVM handler.
//
// NOTE: this module is named "console", not "con" - on Windows, "con" is a
// reserved device name and a file named con.c cannot be opened by the compiler.
#pragma once
#include "efi.h"

void con_init(EFI_SYSTEM_TABLE *st);      // call once at startup
void print(const CHAR16 *s);              // print a UTF-16 string
void print_hex(UINT64 v);                 // print 0x................ (16 digits)
void print_line(const CHAR16 *label, UINT64 v);  // label + hex + newline
void print_ascii(const char *s, UINTN n); // print n bytes of ASCII (for guest data)

// Raw COM1 serial output - works with no firmware (e.g. after ExitBootServices,
// when the UEFI console is gone). QEMU's -serial captures it.
void serial_puts(const char *s);
void serial_ascii(const char *s, UINTN n);
