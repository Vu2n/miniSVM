// main.c - A minimal AMD-V (SVM) hypervisor that boots as a UEFI application.
//
// Milestones (built up incrementally; see README):
//   M0  boot as a UEFI app and print a banner        <-- you are here
//   M1  detect AMD-V (SVM) via CPUID
//   M2  enter SVM mode
//   M3  VMRUN a tiny guest and handle its #VMEXIT
//
// Because we boot via UEFI, the CPU is already in 64-bit long mode with paging
// enabled when efi_main runs. That lets us skip ~500 lines of real-mode ->
// protected-mode -> long-mode bootloader assembly and focus on the hypervisor.

#include "efi.h"
#include "cpu.h"
#include "console.h"
#include "svm.h"

// The linker (with /NODEFAULTLIB) has no CRT, but the compiler may still emit
// calls to memset/memcpy for struct initialization. Provide our own. (We build
// with /Oi- so these are real calls, not inlined intrinsics.)
void *memset(void *dst, int c, UINTN n) {
    unsigned char *p = (unsigned char *)dst;
    while (n--) *p++ = (unsigned char)c;
    return dst;
}
void *memcpy(void *dst, const void *src, UINTN n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--) *d++ = *s++;
    return dst;
}

// ---- Entry point -----------------------------------------------------------
// True if CPUID leaf 0 reports our fake vendor - i.e. we are ALREADY running as
// a guest under a resident miniSVM. Used to make re-entry (the firmware boot
// manager re-launching us) a harmless no-op instead of a re-virtualization mess.
static BOOLEAN already_virtualized(void) {
    int regs[4];
    __cpuid(regs, 0x40000000);   // standard hypervisor CPUID leaf
    UINT32 parts[3] = { (UINT32)regs[1], (UINT32)regs[2], (UINT32)regs[3] };
    char v[12];
    memcpy(v, parts, 12);
    static const char m[12] =
        { 'M','i','n','i','S','V','M',' ','H','y','p','r' };
    for (int i = 0; i < 12; i++)
        if (v[i] != m[i]) return FALSE;
    return TRUE;
}

// Find \EFI\Microsoft\Boot\bootmgfw.efi on some volume, build a full device path
// to it, and LoadImage+StartImage it. Called AFTER we go resident, so Windows
// loads and runs as a guest of our hypervisor. Returns only on failure.
static BOOLEAN chainload_windows(EFI_HANDLE image, EFI_BOOT_SERVICES *bs) {
    static EFI_GUID fsGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    static EFI_GUID dpGuid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    static CHAR16 winPath[] = L"\\EFI\\Microsoft\\Boot\\bootmgfw.efi";

    EFI_HANDLE handles[64];
    UINTN size = sizeof(handles);
    if (EFI_ERROR(bs->LocateHandle(ByProtocol, &fsGuid, NULL, &size, handles))) {
        serial_puts("[chain] LocateHandle(SimpleFS) failed\r\n");
        return FALSE;
    }
    UINTN count = size / sizeof(EFI_HANDLE);

    UINTN nchars = 0;
    while (winPath[nchars]) nchars++;
    UINTN nameBytes = (nchars + 1) * 2;             // include NUL
    UINTN fileNodeLen = 4 + nameBytes;

    for (UINTN i = 0; i < count; i++) {
        EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &fsGuid, (VOID **)&fs)) || !fs)
            continue;
        EFI_FILE_PROTOCOL *root = 0;
        if (EFI_ERROR(fs->OpenVolume(fs, &root)) || !root)
            continue;
        EFI_FILE_PROTOCOL *file = 0;
        EFI_STATUS s = root->Open(root, &file, winPath, EFI_FILE_MODE_READ, 0);
        if (EFI_ERROR(s) || !file) { root->Close(root); continue; }
        file->Close(file);
        root->Close(root);

        // This volume has bootmgfw.efi. Build: <device path> + <file node> + END.
        EFI_DEV_PATH_HDR *devPath = 0;
        if (EFI_ERROR(bs->HandleProtocol(handles[i], &dpGuid, (VOID **)&devPath)) || !devPath) {
            serial_puts("[chain] no device path on the Windows volume\r\n");
            return FALSE;
        }
        UINT8 *p = (UINT8 *)devPath;                // walk to the end node
        for (;;) {
            UINT16 nlen = (UINT16)(p[2] | (p[3] << 8));
            if (p[0] == 0x7F) break;
            p += nlen;
        }
        UINTN devLen = (UINTN)(p - (UINT8 *)devPath);

        static UINT8 full[1024];
        if (devLen + fileNodeLen + 4 > sizeof(full)) return FALSE;
        UINT8 *w = full;
        memcpy(w, devPath, devLen);   w += devLen;  // device portion
        w[0] = 0x04; w[1] = 0x04;                    // MEDIA / FILEPATH node
        w[2] = (UINT8)(fileNodeLen & 0xFF);
        w[3] = (UINT8)(fileNodeLen >> 8);
        memcpy(w + 4, winPath, nameBytes); w += fileNodeLen;
        w[0] = 0x7F; w[1] = 0xFF; w[2] = 4; w[3] = 0; // END node

        EFI_HANDLE winImg = 0;
        if (EFI_ERROR(bs->LoadImage(FALSE, image, full, NULL, 0, &winImg))) {
            serial_puts("[chain] LoadImage(bootmgfw.efi) failed\r\n");
            return FALSE;
        }
        serial_puts("[chain] launching Windows Boot Manager as our guest...\r\n");
        bs->StartImage(winImg, NULL, NULL);         // normally never returns
        serial_puts("[chain] Windows Boot Manager returned (unexpected)\r\n");
        return TRUE;
    }
    serial_puts("[chain] bootmgfw.efi not found on any volume\r\n");
    return FALSE;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    (void)ImageHandle;
    con_init(SystemTable);
    cpu_enable_sse();

    // Immediately probe COM1 so we can confirm serial capture works at all,
    // before anything else. If you see this line in PuTTY / the serial log,
    // capture is good; if not, the VM's serial-port config is the problem.
    serial_puts("\r\n[miniSVM] ===== COM1 serial is alive; hypervisor loading =====\r\n");

    // Re-entry: if the boot manager re-launches us after we already went
    // resident, don't set the hypervisor up again - just hand control back so
    // the boot flow continues to the next option.
    if (already_virtualized()) {
        print(L"[miniSVM] already resident; returning control to the boot manager.\r\n");
        return EFI_SUCCESS;
    }

    print(L"\r\n");
    print(L"=====================================================\r\n");
    print(L"  miniSVM - a minimal AMD-V hypervisor               \r\n");
    print(L"=====================================================\r\n");
    print(L"\r\n");
    print(L"[M0] UEFI application started. CPU is in long mode.\r\n");
    print_line(L"[M0] Firmware revision : ", SystemTable->FirmwareRevision);

    // --- M1: detect AMD-V --------------------------------------------------
    print(L"\r\n[M1] Detecting AMD-V (SVM)...\r\n");
    svm_caps caps = svm_detect();
    if (!caps.supported) {
        print(L"[M1] FAIL: this CPU does not report SVM (are you on Intel, or\r\n");
        print(L"          is nested virtualization disabled in the VM?).\r\n");
        goto done;
    }
    print(L"[M1] OK: SVM is supported by the CPU.\r\n");
    print_line(L"[M1] TLB address-space IDs (ASIDs): ", caps.n_asids);
    if (caps.disabled_locked) {
        print(L"[M1] FAIL: SVM is disabled and locked by firmware. Enable\r\n");
        print(L"          'Virtualize AMD-V/RVI' (nested virt) and retry.\r\n");
        goto done;
    }
    print(L"[M1] OK: SVM is available for use.\r\n");

    // --- M2: enter SVM mode ------------------------------------------------
    print(L"\r\n[M2] Enabling SVM (EFER.SVME) and host save area...\r\n");
    if (!svm_enable(SystemTable->BootServices)) {
        print(L"[M2] FAIL: could not enable SVM.\r\n");
        goto done;
    }
    print(L"[M2] OK: SVM enabled; VM_HSAVE_PA programmed.\r\n");

    // --- M3-M7: run a guest through a VMEXIT dispatch loop ------------------
    print(L"\r\n[M5/M6/M7] Launching guest: CPUID spoof, memory R/W, nested paging...\r\n");
    svm_result res = svm_run(SystemTable->BootServices);
    if (!res.launched) {
        print(L"[M3] FAIL: could not allocate/build the VMCB.\r\n");
        goto done;
    }
    if (res.exit_code == ~0ull) {
        print(L"[M3] FAIL: VMEXIT_INVALID - guest state failed a consistency\r\n");
        print(L"          check. The VMCB layout or a field value is wrong.\r\n");
        goto done;
    }
    print_line(L"[M4] guest exits handled : ", res.exits);
    print_line(L"[M4] final #VMEXIT code   : ", res.exit_code);
    print_line(L"[M4] guest RIP at exit    : ", res.guest_rip);
    print(L"\r\n[*] SUCCESS - the guest ran and we serviced its exits.\r\n");

    // --- M9: self-virtualization -------------------------------------------
    // Up to here the "guest" was a separate blob. Now we virtualize OUR OWN
    // running code: after svm_go_resident returns, efi_main itself is a guest
    // and the hypervisor is resident underneath it. We prove it by running
    // CPUID on ourselves - it should come back spoofed.
    print(L"\r\n[M9] Self-virtualizing: putting THIS code into a guest...\r\n");
    svm_go_resident(ImageHandle, SystemTable->BootServices);

    // From here on, efi_main is executing as a guest under our own hypervisor.
    // We stay transparent: CPUID leaf 0 still reports the REAL CPU vendor (so a
    // future guest OS behaves), while the standard hypervisor leaf 0x40000000
    // reveals us. Show both.
    {
        int r0[4];
        __cpuid(r0, 0);
        UINT32 v0[3] = { (UINT32)r0[1], (UINT32)r0[3], (UINT32)r0[2] };
        char real[13];
        memcpy(real, v0, 12); real[12] = 0;

        int rh[4];
        __cpuid(rh, 0x40000000);
        UINT32 vh[3] = { (UINT32)rh[1], (UINT32)rh[2], (UINT32)rh[3] };
        char sig[13];
        memcpy(sig, vh, 12); sig[12] = 0;

        print(L"[M9] CPUID leaf 0 vendor (real, unchanged) : ");
        print_ascii(real, 12);
        print(L"\r\n[M9] CPUID leaf 0x40000000 (hypervisor)    : ");
        print_ascii(sig, 12);
        print(L"\r\n[M9] ^ leaf 0x40000000 = 'MiniSVM Hypr' means efi_main is a\r\n");
        print(L"     guest and our hypervisor is resident beneath it.\r\n");
    }

    // --- M11: chainload Windows as our guest -------------------------------
    // We are resident. Directly load and start the Windows Boot Manager, so
    // Windows boots INSIDE our guest session (deterministic - no reliance on the
    // firmware boot menu). When Windows later calls ExitBootServices and reclaims
    // boot memory, our hypervisor (in EfiRuntimeServicesData) survives.
    print(L"\r\n[M11] Hypervisor resident. Chainloading Windows as our guest...\r\n");
    serial_puts("[M11] resident; chainloading Windows Boot Manager...\r\n");
    chainload_windows(ImageHandle, SystemTable->BootServices);

    // Fell through: Windows wasn't found/launched. Hand back to the boot manager.
    print(L"[M11] Chainload failed; returning to boot manager (see serial log).\r\n");
    return EFI_SUCCESS;

done:
    // Reached only on an early failure (no hypervisor installed): halt so the
    // message stays on screen.
    for (;;) __halt();
    return EFI_SUCCESS;
}
