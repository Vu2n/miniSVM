// svm.c - AMD-V (SVM) core implementation.
#include "svm.h"
#include "cpu.h"
#include "console.h"
#include "npt.h"

// ===========================================================================
//  VMCB layout (AMD APM Vol.2, Appendix B)
//
//  The Virtual Machine Control Block is one 4 KiB page: a 0x400-byte Control
//  Area followed by a 0xC00-byte State Save Area. We describe it with structs
//  whose reserved[] padding reproduces the spec offsets exactly; a compile-time
//  check on sizeof(vmcb) guards the most common layout mistakes.
// ===========================================================================
#pragma pack(push, 1)

// One segment register as stored in the VMCB save area.
typedef struct {
    UINT16 selector;
    UINT16 attrib;    // packed descriptor attributes (12 bits used)
    UINT32 limit;
    UINT64 base;
} vmcb_seg;

typedef struct {
    UINT16 intercept_cr_read;      // 0x000
    UINT16 intercept_cr_write;     // 0x002
    UINT16 intercept_dr_read;      // 0x004
    UINT16 intercept_dr_write;     // 0x006
    UINT32 intercept_exceptions;   // 0x008
    UINT32 intercept_vec3;         // 0x00C  (INTR, CPUID, HLT, ...)
    UINT32 intercept_vec4;         // 0x010  (VMRUN=bit0, VMMCALL=bit1, ...)
    UINT32 intercept_vec5;         // 0x014
    UINT8  reserved1[0x03C - 0x018];
    UINT16 pause_filter_thresh;    // 0x03C
    UINT16 pause_filter_count;     // 0x03E
    UINT64 iopm_base_pa;           // 0x040
    UINT64 msrpm_base_pa;          // 0x048
    UINT64 tsc_offset;             // 0x050
    UINT32 guest_asid;             // 0x058
    UINT8  tlb_control;            // 0x05C
    UINT8  reserved2[3];
    UINT64 v_intr;                 // 0x060
    UINT64 int_shadow;             // 0x068
    UINT64 exit_code;              // 0x070
    UINT64 exit_info1;             // 0x078
    UINT64 exit_info2;             // 0x080
    UINT64 exit_int_info;          // 0x088
    UINT64 np_enable;              // 0x090
    UINT64 reserved3;              // 0x098
    UINT64 reserved4;              // 0x0A0
    UINT64 eventinj;               // 0x0A8
    UINT64 n_cr3;                  // 0x0B0
    UINT64 lbr_virt;               // 0x0B8
    UINT32 clean_bits;             // 0x0C0
    UINT32 reserved5;              // 0x0C4
    UINT64 nrip;                   // 0x0C8
    UINT8  reserved6[0x400 - 0x0D0];
} vmcb_control;

typedef struct {
    vmcb_seg es;    // 0x400
    vmcb_seg cs;    // 0x410
    vmcb_seg ss;    // 0x420
    vmcb_seg ds;    // 0x430
    vmcb_seg fs;    // 0x440
    vmcb_seg gs;    // 0x450
    vmcb_seg gdtr;  // 0x460
    vmcb_seg ldtr;  // 0x470
    vmcb_seg idtr;  // 0x480
    vmcb_seg tr;    // 0x490
    UINT8  reserved1[0x0CB - 0x0A0];
    UINT8  cpl;                     // 0x4CB
    UINT8  reserved2[4];
    UINT64 efer;                    // 0x4D0
    UINT8  reserved3[0x148 - 0x0D8];
    UINT64 cr4;                     // 0x548
    UINT64 cr3;                     // 0x550
    UINT64 cr0;                     // 0x558
    UINT64 dr7;                     // 0x560
    UINT64 dr6;                     // 0x568
    UINT64 rflags;                  // 0x570
    UINT64 rip;                     // 0x578
    UINT8  reserved4[0x1D8 - 0x180];
    UINT64 rsp;                     // 0x5D8
    UINT8  reserved5[0x1F8 - 0x1E0];
    UINT64 rax;                     // 0x5F8
    UINT64 star;                    // 0x600
    UINT64 lstar;                   // 0x608
    UINT64 cstar;                   // 0x610
    UINT64 sfmask;                  // 0x618
    UINT64 kernel_gs_base;          // 0x620
    UINT64 sysenter_cs;             // 0x628
    UINT64 sysenter_esp;            // 0x630
    UINT64 sysenter_eip;            // 0x638
    UINT64 cr2;                     // 0x640
    UINT8  reserved6[0x268 - 0x248];
    UINT64 g_pat;                   // 0x668
    UINT8  reserved7[0xC00 - 0x270];
} vmcb_save;

typedef struct {
    vmcb_control control;   // 0x000 .. 0x3FF
    vmcb_save    save;      // 0x400 .. 0xFFF
} vmcb;

#pragma pack(pop)

// Compile-time guardrail: any offset mistake almost always changes this size.
typedef char vmcb_is_one_page[(sizeof(vmcb) == 0x1000) ? 1 : -1];

// Host descriptor snapshot filled by capture_host_desc (see vmrun.asm).
#pragma pack(push, 1)
typedef struct {
    UINT16 cs, ss, ds, es, fs, gs, tr, ldtr;
    UINT16 gdt_limit; UINT64 gdt_base;
    UINT16 idt_limit; UINT64 idt_base;
} host_desc;
#pragma pack(pop)

// The guest's general-purpose registers (RAX/RSP/RIP live in the VMCB instead).
// svm_launch loads these before VMRUN and saves them back after, so the guest
// keeps its register state across exits. The field order MUST match vmrun.asm.
#pragma pack(push, 1)
typedef struct {
    UINT64 rbx, rcx, rdx, rsi, rdi, rbp;
    UINT64 r8, r9, r10, r11, r12, r13, r14, r15;
} guest_gprs;
#pragma pack(pop)

// Assembly helpers implemented in vmrun.asm.
extern void capture_host_desc(host_desc *p);
extern void svm_launch(UINT64 guest_vmcb_pa, UINT64 host_vmcb_pa, guest_gprs *g);
extern void guest_entry(void);
extern UINT64 hv_resident(UINT64 guest_vmcb_pa, UINT64 host_vmcb_pa,
                          guest_gprs *g, UINT64 host_stack_top);

// VMCB intercept bits and exit codes we care about.
#define INTERCEPT_CPUID   (1u << 18)  // vector 3: intercept the CPUID instruction
#define INTERCEPT_VMRUN   (1u << 0)   // vector 4: required for VMRUN to be legal
#define INTERCEPT_VMMCALL (1u << 1)   // vector 4: intercept VMMCALL
#define VMEXIT_CPUID      0x072ull
#define VMEXIT_VMMCALL    0x081ull
#define VMEXIT_NPF        0x400ull     // nested page fault (guest-physical)
#define VMEXIT_INVALID    (~0ull)     // guest state failed consistency checks

// Guest -> hypervisor calls: the request number is passed in the guest's RAX.
#define HC_EXIT           0x00        // guest asks us to stop the run loop
#define HC_REPORT_VENDOR  0x10        // guest reports the CPUID vendor it saw
                                      // (in RBX, RDX, RCX - CPUID.0 layout)
#define HC_PROCESS        0x20        // guest hands us a buffer pointer in RBX;
                                      // we read it and write a response back
#define HC_REPORT         0x21        // guest reports 8 bytes (in RDX) it read
                                      // back from its buffer after we changed it
#define HC_NPT_DONE       0x30        // guest wrote via its NPT-redirected page
                                      // (pointer was pre-loaded into guest RDI)

// Length in bytes of the instructions we advance past (fallback when the CPU
// does not report the next RIP via NRIPS).
#define VMMCALL_LEN       3
#define CPUID_LEN         2

// The fake CPU vendor we feed the guest via CPUID leaf 0 (exactly 12 chars).
static const char FAKE_VENDOR[12] =
    { 'M','i','n','i','S','V','M',' ','H','y','p','r' };

// Flat 64-bit segment attributes (packed VMCB form).
#define SEG_ATTR_CODE64   0x029B   // type=code r/x/accessed, S, P, L=1, D=0
#define SEG_ATTR_DATA     0x0093   // type=data r/w/accessed, S, P

// ---- M1: capability detection ---------------------------------------------
//
// Two questions to answer before we can virtualize:
//   1. Does the CPU implement SVM?   -> CPUID.80000001h:ECX[2]
//   2. Is it available (not disabled+locked by firmware)? -> VM_CR MSR + CPUID
svm_caps svm_detect(void) {
    svm_caps c = {0};
    int regs[4];  // [0]=EAX [1]=EBX [2]=ECX [3]=EDX

    // Highest supported extended CPUID leaf.
    __cpuid(regs, 0x80000000);
    unsigned max_ext = (unsigned)regs[0];
    if (max_ext < CPUID_EXT_FEATURES)
        return c;  // no extended feature leaf -> definitely no SVM

    // SVM present?  CPUID.80000001h:ECX bit 2.
    __cpuid(regs, (int)CPUID_EXT_FEATURES);
    if (!(regs[2] & (1u << 2)))
        return c;
    c.supported = 1;

    // SVM feature leaf (80000000Ah): EBX[7:0]... actually EBX = NASID count.
    if (max_ext >= CPUID_SVM_FEATURES) {
        __cpuid(regs, (int)CPUID_SVM_FEATURES);
        c.n_asids = (unsigned)regs[1];
    }

    // Is SVM turned off in firmware?  VM_CR.SVMDIS (bit 4).
    UINT64 vm_cr = __readmsr(MSR_VM_CR);
    if (vm_cr & VM_CR_SVMDIS) {
        // Disabled. If the SVM-lock bit (CPUID.8000000Ah:EDX[2]) is set, the
        // BIOS locked it and we cannot turn it back on. Otherwise we could
        // clear SVMDIS ourselves (left as an exercise / handled at enable time).
        __cpuid(regs, (int)CPUID_SVM_FEATURES);
        BOOLEAN locked = (regs[3] & (1u << 2)) != 0;
        c.disabled_locked = locked;
        c.usable = !locked;
    } else {
        c.usable = 1;
    }
    return c;
}

// Allocate one zeroed, page-aligned page. UEFI identity-maps memory during boot
// services, so the returned address is usable both as a pointer and as the
// physical address the CPU needs (for VMCB / host-save-area / guest stack).
static UINT64 alloc_page(EFI_BOOT_SERVICES *bs) {
    UINT64 addr = 0;
    // EfiRuntimeServicesData so these pages survive an OS's ExitBootServices.
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &addr)))
        return 0;
    UINT8 *p = (UINT8 *)addr;
    for (UINTN i = 0; i < 4096; i++) p[i] = 0;
    return addr;
}

// ---- M2: enter SVM mode ---------------------------------------------------
BOOLEAN svm_enable(EFI_BOOT_SERVICES *bs) {
    // If firmware left SVM disabled but unlocked, turn it back on.
    UINT64 vm_cr = __readmsr(MSR_VM_CR);
    if (vm_cr & VM_CR_SVMDIS)
        __writemsr(MSR_VM_CR, vm_cr & ~VM_CR_SVMDIS);

    // EFER.SVME = 1 enables the SVM instruction set (VMRUN, VMSAVE, ...).
    __writemsr(MSR_EFER, __readmsr(MSR_EFER) | EFER_SVME);
    if (!(__readmsr(MSR_EFER) & EFER_SVME))
        return FALSE;

    // The host state-save area: the CPU stashes host state here across VMRUN.
    UINT64 hsave = alloc_page(bs);
    if (!hsave)
        return FALSE;
    __writemsr(MSR_VM_HSAVE_PA, hsave);
    return TRUE;
}

// ---- M3: build a VMCB and run the guest -----------------------------------
svm_result svm_run(EFI_BOOT_SERVICES *bs) {
    svm_result r = {0};

    UINT64 guest_pa = alloc_page(bs);   // the guest VMCB
    UINT64 host_pa  = alloc_page(bs);   // scratch VMCB for host extra-state
    UINT64 stack    = alloc_page(bs);   // a private stack for the guest
    if (!guest_pa || !host_pa || !stack)
        return r;

    vmcb *v = (vmcb *)guest_pa;

    // Snapshot the running host so we can hand the guest valid segment/table
    // state. FS/GS/TR/LDTR are copied by VMSAVE inside svm_launch, so we only
    // fill CS/SS/DS/ES and the descriptor tables here.
    host_desc hd;
    capture_host_desc(&hd);

    v->save.cs.selector = hd.cs; v->save.cs.attrib = SEG_ATTR_CODE64;
    v->save.cs.limit = 0xFFFFFFFF; v->save.cs.base = 0;
    v->save.ss.selector = hd.ss; v->save.ss.attrib = SEG_ATTR_DATA;
    v->save.ss.limit = 0xFFFFFFFF; v->save.ss.base = 0;
    v->save.ds.selector = hd.ds; v->save.ds.attrib = SEG_ATTR_DATA;
    v->save.ds.limit = 0xFFFFFFFF; v->save.ds.base = 0;
    v->save.es.selector = hd.es; v->save.es.attrib = SEG_ATTR_DATA;
    v->save.es.limit = 0xFFFFFFFF; v->save.es.base = 0;

    v->save.gdtr.base = hd.gdt_base; v->save.gdtr.limit = hd.gdt_limit;
    v->save.idtr.base = hd.idt_base; v->save.idtr.limit = hd.idt_limit;

    // Control registers / EFER: give the guest the same paging environment as
    // the host (same CR3 -> the guest sees the same address space, so its code
    // and stack are already mapped). EFER carries SVME/LME/LMA, all required.
    v->save.cr0 = __readcr0();
    v->save.cr3 = __readcr3();
    v->save.cr4 = __readcr4();
    v->save.efer = __readmsr(MSR_EFER);

    v->save.rflags = 0x2;                       // bit 1 reserved-1; IF cleared
    v->save.rip = (UINT64)&guest_entry;         // where the guest starts
    v->save.rsp = stack + 4096;                 // top of the guest stack
    v->save.rax = 0;
    v->save.cpl = 0;

    // Control area: a nonzero ASID is required. Intercept CPUID (so we can spoof
    // it), plus VMRUN (mandatory) and VMMCALL (so hypercalls trap back to us).
    v->control.guest_asid = 1;
    v->control.intercept_vec3 = INTERCEPT_CPUID;
    v->control.intercept_vec4 = INTERCEPT_VMRUN | INTERCEPT_VMMCALL;
    // (nested paging is configured below, in the M7 block)

    // For contrast, show the REAL CPU vendor (what the host sees) before the
    // guest runs and gets fed our fake one.
    {
        int hr[4];
        __cpuid(hr, 0);
        UINT32 real[3] = { (UINT32)hr[1], (UINT32)hr[3], (UINT32)hr[2] };
        char vendor[13];
        memcpy(vendor, real, 12);
        vendor[12] = 0;
        print(L"      [hv] real host CPU vendor : ");
        print_ascii(vendor, 12);
        print(L"\r\n");
    }

    // --- M7: nested paging -------------------------------------------------
    // Give the guest its own physical address space. We identity-map guest-
    // physical == host-physical (so the code/stack/buffers above still work),
    // but redirect ONE guest-physical page to a different host page we own.
    UINT64 npt_src = alloc_page(bs);   // guest-physical page the guest will write
    UINT64 npt_dst = alloc_page(bs);   // host page the NPT secretly redirects to
    if (!npt_src || !npt_dst) return r;
    // Distinct markers so we can prove where the guest's write actually lands.
    memcpy((void *)npt_src, "HOSTPAGE", 8);   // host page A: should stay untouched
    memset((void *)npt_dst, 0, 8);            // host page R: guest's write lands here

    UINT64 npt_root = npt_build(bs, npt_src, npt_dst);
    if (!npt_root) return r;
    v->control.np_enable = 1;                 // turn nested paging ON
    v->control.n_cr3 = npt_root;              // NPT top-level table (host-physical)
    v->save.g_pat = 0x0007040600070406ull;    // default PAT (NPT uses guest PAT)

    // The guest's general-purpose register context, persisted across exits.
    // RDI carries the guest-physical address whose NPT mapping we hijacked.
    guest_gprs gprs = {0};
    gprs.rdi = npt_src;

    r.launched = TRUE;

    // --- VMEXIT dispatch loop ----------------------------------------------
    // Enter the guest, handle whatever made it exit, and resume - until the
    // guest issues the EXIT hypercall (or something unexpected happens).
    for (;;) {
        svm_launch(guest_pa, host_pa, &gprs);   // VMRUN; returns on #VMEXIT

        UINT64 code = v->control.exit_code;
        r.exit_code = code;
        r.guest_rip = v->save.rip;

        if (code == VMEXIT_INVALID)
            return r;                            // malformed guest state

        if (code == VMEXIT_NPF) {
            // A nested page fault: the guest touched a guest-physical address
            // our NPT doesn't map. Report the faulting GPA and stop.
            print(L"      [hv] nested page fault at GPA ");
            print_hex(v->control.exit_info2);
            print(L"\r\n");
            return r;
        }

        if (code == VMEXIT_CPUID) {
            // The guest executed CPUID. Compute the real answer, then - for leaf
            // 0 - overwrite the vendor string with our fake one before handing
            // the result back. CPUID returns EAX/EBX/ECX/EDX; in the guest,
            // EAX is VMCB.RAX and EBX/ECX/EDX live in our register context.
            UINT32 leaf    = (UINT32)v->save.rax;
            UINT32 subleaf = (UINT32)gprs.rcx;
            int regs[4];
            __cpuidex(regs, (int)leaf, (int)subleaf);
            if (leaf == 0) {
                UINT32 fb, fd, fc;
                memcpy(&fb, &FAKE_VENDOR[0], 4);
                memcpy(&fd, &FAKE_VENDOR[4], 4);
                memcpy(&fc, &FAKE_VENDOR[8], 4);
                regs[1] = (int)fb;   // EBX
                regs[3] = (int)fd;   // EDX
                regs[2] = (int)fc;   // ECX
            }
            v->save.rax = (UINT32)regs[0];       // EAX (zero-extends to 64-bit)
            gprs.rbx    = (UINT32)regs[1];
            gprs.rcx    = (UINT32)regs[2];
            gprs.rdx    = (UINT32)regs[3];
            r.exits++;
            v->save.rip = v->control.nrip ? v->control.nrip
                                          : v->save.rip + CPUID_LEN;
            continue;
        }

        if (code == VMEXIT_VMMCALL) {
            UINT64 hc = v->save.rax;             // hypercall number in guest RAX
            if (hc == HC_EXIT) {
                print(L"      [hv] guest requested EXIT.\r\n");
                return r;
            }
            if (hc == HC_REPORT_VENDOR) {
                // Guest hands back the vendor it observed (CPUID.0 layout:
                // EBX, EDX, ECX). Print it - it should be our fake string.
                UINT32 parts[3] = { (UINT32)gprs.rbx, (UINT32)gprs.rdx,
                                    (UINT32)gprs.rcx };
                char vendor[13];
                memcpy(vendor, parts, 12);
                vendor[12] = 0;
                print(L"      [hv] guest's CPUID vendor : ");
                print_ascii(vendor, 12);
                print(L"   <-- spoofed!\r\n");
            } else if (hc == HC_PROCESS) {
                // The guest gave us a pointer into its memory (RBX). Read what
                // it wrote there, then overwrite it with our own response. In
                // this shared-address-space design guest-virtual == host-virtual,
                // so we can dereference the pointer directly. (With nested
                // paging you would translate guest-physical -> host-physical.)
                char *gbuf = (char *)gprs.rbx;
                char in[9];
                memcpy(in, gbuf, 8);
                in[8] = 0;
                print(L"      [hv] read from guest memory : ");
                print_ascii(in, 8);
                print(L"\r\n");

                static const char resp[8] =
                    { 'P', 'O', 'N', 'G', ' ', 'H', 'V', '!' };
                memcpy(gbuf, resp, 8);   // write into the guest's buffer
                print(L"      [hv] wrote to guest memory  : PONG HV!\r\n");
            } else if (hc == HC_REPORT) {
                // Guest passes back the 8 bytes it read from its buffer AFTER we
                // modified it (in RDX). It should be our response - proving the
                // guest observed the hypervisor's write.
                char out[9];
                memcpy(out, &gprs.rdx, 8);
                out[8] = 0;
                print(L"      [hv] guest read back         : ");
                print_ascii(out, 8);
                print(L"   <-- our write!\r\n");
            } else if (hc == HC_NPT_DONE) {
                // The guest just wrote to guest-physical `npt_src` (via RDI).
                // Our NPT redirected that page to host page `npt_dst`, so the
                // write should appear in npt_dst - while host page npt_src still
                // holds our original marker.
                char a[9], d[9];
                memcpy(a, (void *)npt_src, 8); a[8] = 0;
                memcpy(d, (void *)npt_dst, 8); d[8] = 0;
                print(L"      [hv] guest wrote to guest-physical ");
                print_hex(npt_src);
                print(L"\r\n      [hv]   host page A (");
                print_hex(npt_src);
                print(L") still: ");
                print_ascii(a, 8);
                print(L"  (untouched)\r\n      [hv]   host page R (");
                print_hex(npt_dst);
                print(L") now  : ");
                print_ascii(d, 8);
                print(L"  <-- guest's write landed here via NPT!\r\n");
            } else {
                print(L"      [hv] unknown hypercall, RAX = ");
                print_hex(hc);
                print(L"\r\n");
            }
            r.exits++;
            v->save.rip = v->control.nrip ? v->control.nrip
                                          : v->save.rip + VMMCALL_LEN;
            continue;
        }

        // Any other exit code: stop and let the caller report it.
        return r;
    }
}

// ---- M9: self-virtualization ----------------------------------------------
//
// Debug output straight to the COM1 serial port (0x3F8). Unlike the UEFI
// console, raw port I/O works from the resident host context (we must NOT call
// firmware while self-virtualized - the guest owns that). QEMU's -serial routes
// COM1 to our log; OVMF already initialized the port.
#define COM1 0x3F8
#define COM2 0x2F8
static void dbg_init_port(unsigned short p) {
    __outbyte(p + 1, 0x00);
    __outbyte(p + 3, 0x80);
    __outbyte(p + 0, 0x01);
    __outbyte(p + 1, 0x00);
    __outbyte(p + 3, 0x03);
    __outbyte(p + 2, 0xC7);
    __outbyte(p + 4, 0x0B);
}
static void dbg_putc_port(unsigned short p, char c) {
    for (int i = 0; i < 100000; i++)
        if (__inbyte(p + 5) & 0x20) break;
    __outbyte(p, (unsigned char)c);
}
// Emit to BOTH COM1 and COM2 (we don't know which the VM's serial file uses).
static void dbg_putc(char c) {
    static BOOLEAN inited = FALSE;
    if (!inited) { dbg_init_port(COM1); dbg_init_port(COM2); inited = TRUE; }
    dbg_putc_port(COM1, c);
    dbg_putc_port(COM2, c);
}
static void dbg_puts(const char *s) { while (*s) dbg_putc(*s++); }
static void dbg_hex(const char *label, UINT64 v) {
    dbg_puts(label);
    dbg_puts("0x");
    for (int i = 0; i < 16; i++) {
        int nib = (int)((v >> ((15 - i) * 4)) & 0xF);
        dbg_putc((char)(nib < 10 ? '0' + nib : 'A' + (nib - 10)));
    }
    dbg_puts("\r\n");
}

// ---- Virtual Machine Introspection ----------------------------------------
//
// Translate a GUEST virtual address to a host pointer by walking the guest's
// own page tables (root = guest CR3 from the VMCB), then the NPT (identity, so
// GPA == HPA). Our host CR3 identity-maps all RAM, so the returned physical
// address is directly dereferenceable. Returns NULL if not mapped.
static void *guest_va_to_host(vmcb *v, UINT64 gva) {
    UINT64 M   = 0x000FFFFFFFFFF000ull;               // 4 KiB addr mask
    UINT64 lim = 16ull << 30;                         // our NPT/host map limit
    UINT64 a = v->save.cr3 & M;
    if (a >= lim) return NULL;
    UINT64 e = ((UINT64 *)a)[(gva >> 39) & 0x1FF];    // PML4E
    if (!(e & 1)) return NULL;
    a = e & M; if (a >= lim) return NULL;
    e = ((UINT64 *)a)[(gva >> 30) & 0x1FF];           // PDPTE
    if (!(e & 1)) return NULL;
    if (e & 0x80) { a = (e & 0x000FFFFFC0000000ull) | (gva & 0x3FFFFFFF); return a < lim ? (void *)a : NULL; }
    a = e & M; if (a >= lim) return NULL;
    e = ((UINT64 *)a)[(gva >> 21) & 0x1FF];           // PDE
    if (!(e & 1)) return NULL;
    if (e & 0x80) { a = (e & 0x000FFFFFFFE00000ull) | (gva & 0x1FFFFF); return a < lim ? (void *)a : NULL; }
    a = e & M; if (a >= lim) return NULL;
    e = ((UINT64 *)a)[(gva >> 12) & 0x1FF];           // PTE
    if (!(e & 1)) return NULL;
    a = (e & M) | (gva & 0xFFF);
    return a < lim ? (void *)a : NULL;
}

// Read a few well-known fields out of KUSER_SHARED_DATA (a fixed kernel address
// that every Windows maps) - proof the hypervisor can read the live OS's own
// memory. NtSystemRoot ("C:\Windows") and the version live at stable offsets.
static void hv_vmi_windows(vmcb *v) {
    UINT64 KUSD = 0xFFFFF78000000000ull;
    UINT16 *root = (UINT16 *)guest_va_to_host(v, KUSD + 0x030);
    UINT32 *maj  = (UINT32 *)guest_va_to_host(v, KUSD + 0x26C);
    UINT32 *min  = (UINT32 *)guest_va_to_host(v, KUSD + 0x270);

    dbg_puts("\r\n[VMI] === Hypervisor reading LIVE Windows kernel memory ===\r\n");
    if (root) {
        dbg_puts("[VMI]   NtSystemRoot   : ");
        for (int i = 0; i < 128 && root[i]; i++) dbg_putc((char)root[i]);
        dbg_puts("\r\n");
    }
    if (maj && min) {
        dbg_hex("[VMI]   NtMajorVersion : ", *maj);
        dbg_hex("[VMI]   NtMinorVersion : ", *min);
    }
    dbg_hex("[VMI]   (walked via guest CR3 = ", v->save.cr3);
    dbg_puts("[VMI] === we just read that out of Windows' own address space ===\r\n\r\n");
}

// Return a pointer to the 2 MiB NPT leaf entry (PDE with PS=1) that maps a given
// guest-physical address, so we can change its permissions. NULL if not a 2 MiB
// leaf. The NPT pages are host-physical == host-virtual (identity host CR3).
static UINT64 *npt_2mb_entry(UINT64 n_cr3, UINT64 gpa) {
    UINT64 M = 0x000FFFFFFFFFF000ull;
    UINT64 *pml4 = (UINT64 *)(n_cr3 & M);
    UINT64 e = pml4[(gpa >> 39) & 0x1FF];
    if (!(e & 1)) return NULL;
    UINT64 *pdpt = (UINT64 *)(e & M);
    e = pdpt[(gpa >> 30) & 0x1FF];
    if (!(e & 1) || (e & 0x80)) return NULL;   // must point to a PD, not a 1 GiB leaf
    UINT64 *pd = (UINT64 *)(e & M);
    UINT64 *pde = &pd[(gpa >> 21) & 0x1FF];
    return (*pde & 0x80) ? pde : NULL;         // must be a 2 MiB leaf
}

// hv_handle_exit is called (from vmrun.asm's resident loop) on every #VMEXIT
// while self-virtualized. It stays off the firmware console (the guest owns it);
// it only spoofs CPUID leaf 0 and passes everything else through by advancing
// past the instruction.
void hv_handle_exit(vmcb *v, guest_gprs *g) {
    // Log the guest RIP on the first exits and then periodically. This is the
    // definitive proof of what's running as our guest: once Windows is booting,
    // you'll see kernel addresses (0xFFFFF8...) streaming by. Silence = it
    // stopped; a dump further down = it faulted.
    static UINT64 exits = 0;
    static BOOLEAN kernel_seen = FALSE;
    exits++;
    if (exits <= 32 || (exits & 0x1FFF) == 0)     // first 32, then every 8192
        dbg_hex("[hv] guest rip=", v->save.rip);
    // Milestone: the guest RIP entering the high canonical half means the
    // Windows KERNEL (ntoskrnl) is now executing as our guest. At that point we
    // have proven virtualization, so DISABLE CPUID interception (clean_bits are
    // 0, so the CPU reloads intercepts on the next VMRUN) - the guest then runs
    // at near-native speed and can boot to the desktop. We stay resident (VMRUN
    // intercept + NPT), so Windows is still our guest, just faster.
    static BOOLEAN vmi_done = FALSE;
    static UINT64 kexits = 0;
    // Memory-protection state (used here and in the #NPF handler below).
    static BOOLEAN memprot_on = FALSE;
    static UINT64 *memprot_pde = NULL;
    static UINT64 memprot_base = 0;
    if (v->save.rip >= 0xFFFF800000000000ull) {
        if (!kernel_seen) {
            kernel_seen = TRUE;
            dbg_puts("\r\n[hv] *** Windows KERNEL running as our guest! ***\r\n");
        }
        if (!vmi_done) {
            kexits++;
            // Wait until the kernel has filled in its version (a bit later than
            // NtSystemRoot), then do the full VMI read.
            UINT32 *ver = (UINT32 *)guest_va_to_host(v, 0xFFFFF78000000000ull + 0x26C);
            if (ver && *ver) {
                hv_vmi_windows(v);
                dbg_hex("[hv] exits serviced to reach the Windows kernel: ", exits);

                // FEATURE - NPT memory protection: lock the 2 MiB region holding
                // KUSER_SHARED_DATA read-only, so we catch the next guest write.
                UINT64 kgpa = (UINT64)guest_va_to_host(v, 0xFFFFF78000000000ull);
                UINT64 *pde = kgpa ? npt_2mb_entry(v->control.n_cr3, kgpa) : NULL;
                if (pde && (*pde & 2)) {
                    memprot_pde = pde;
                    memprot_base = kgpa & ~0x1FFFFFull;
                    *pde &= ~2ull;                 // clear R/W -> read-only
                    v->control.tlb_control = 3;    // flush this guest's TLB
                    memprot_on = TRUE;
                    dbg_puts("[MEMPROT] locked KUSER_SHARED_DATA's page READ-ONLY;\r\n");
                    dbg_puts("[MEMPROT] waiting to catch Windows writing to it...\r\n");
                }
                vmi_done = TRUE;
            } else if (kexits > 40000) {
                dbg_puts("[hv] (VMI: kernel data not readable; skipping)\r\n");
                vmi_done = TRUE;
            } else if ((kexits & 0x7FF) == 0) {   // heartbeat so it's never silent
                dbg_hex("[hv] VMI waiting for kernel to map its data; cr3=", v->save.cr3);
            }
            if (vmi_done) {
                dbg_puts("[hv] disabling CPUID intercept -> guest now runs at native speed.\r\n");
                v->control.intercept_vec3 = 0;
            }
        }
    }

    UINT64 code = v->control.exit_code;

    if (code == VMEXIT_CPUID) {
        // Transparent to the guest OS: pass real values through, but (a) set the
        // "hypervisor present" bit on leaf 1, and (b) answer the standard
        // hypervisor leaf 0x40000000 with our signature. Leaf 0's real vendor
        // (AuthenticAMD) is left intact so the OS behaves normally.
        UINT32 leaf = (UINT32)v->save.rax;
        int regs[4];
        __cpuidex(regs, (int)leaf, (int)(UINT32)g->rcx);
        // Stay maximally transparent for a real OS: do NOT set the "hypervisor
        // present" bit (so the OS behaves as on bare metal). Only answer the
        // hypervisor leaf 0x40000000 with our signature (harmless; the OS won't
        // even read it without the present bit, but our own guard uses it).
        if (leaf == 0x40000000) {
            UINT32 a, b, c;
            memcpy(&a, &FAKE_VENDOR[0], 4);
            memcpy(&b, &FAKE_VENDOR[4], 4);
            memcpy(&c, &FAKE_VENDOR[8], 4);
            regs[0] = 0x40000000;                // max hypervisor leaf
            regs[1] = (int)a; regs[2] = (int)b; regs[3] = (int)c;  // "MiniSVM Hypr"
        }
        v->save.rax = (UINT32)regs[0];
        g->rbx = (UINT32)regs[1];
        g->rcx = (UINT32)regs[2];
        g->rdx = (UINT32)regs[3];
        v->save.rip = v->control.nrip ? v->control.nrip
                                      : v->save.rip + CPUID_LEN;
        return;
    }

    if (code == VMEXIT_VMMCALL) {
        v->save.rip = v->control.nrip ? v->control.nrip
                                      : v->save.rip + VMMCALL_LEN;
        return;
    }

    if (code == VMEXIT_NPF) {
        UINT64 fault_gpa = v->control.exit_info2;

        // FEATURE - memory protection: did the guest just write to the page we
        // locked read-only? Catch it red-handed, name the culprit, then release
        // the lock so Windows carries on.
        if (memprot_on && memprot_pde &&
            fault_gpa >= memprot_base && fault_gpa < memprot_base + 0x200000) {
            // Restore R/W FIRST and resume as fast as possible - the guest is
            // paused mid-instruction (maybe in an ISR), so keep the stall tiny.
            *memprot_pde |= 2ull;              // restore R/W
            v->control.tlb_control = 3;        // flush this guest's TLB
            memprot_on = FALSE;
            dbg_puts("[MEMPROT] caught guest write to locked page; RIP=");
            dbg_hex("", v->save.rip);          // one short line, then resume
            return;
        }

        // Otherwise: an unexpected NPF. With a correct, persistent, identity NPT
        // these should not happen for mapped RAM - but if one does, log a few and
        // resume (re-execute). If the SAME guest-physical page faults over and
        // over we are genuinely stuck, so report it and stop.
        static UINT64 last_gpa = ~0ull;
        static UINT32 same = 0, logged = 0;
        UINT64 gpa = v->control.exit_info2;
        if (gpa == last_gpa) {
            if (++same >= 200) {
                dbg_puts("\r\n[NPF] STUCK - same guest-physical page keeps faulting:\r\n");
                dbg_hex("     gpa        : ", gpa);
                dbg_hex("     exit_info1 : ", v->control.exit_info1);
                dbg_hex("     guest RIP  : ", v->save.rip);
                for (;;) __halt();
            }
        } else {
            last_gpa = gpa;
            same = 0;
            if (logged < 32) { dbg_hex("[NPF] gpa ", gpa); logged++; }
        }
        return;   // resume the faulting instruction
    }

    // Truly unexpected exit: report via serial and stop.
    dbg_puts("\r\n[hv] unexpected #VMEXIT while resident - stopping:\r\n");
    dbg_hex("     exit_code  : ", code);
    dbg_hex("     exit_info1 : ", v->control.exit_info1);
    dbg_hex("     exit_info2 : ", v->control.exit_info2);
    dbg_hex("     guest RIP  : ", v->save.rip);
    for (;;) __halt();
}

// Fill a VMCB's guest state save area with the CURRENTLY-running CPU state, so
// the guest is an exact clone of us. RIP/RSP/RFLAGS/RAX are left for
// hv_resident to set from the caller's context.
static void fill_current_cpu_state(vmcb *v) {
    host_desc hd;
    capture_host_desc(&hd);
    v->save.cs.selector = hd.cs; v->save.cs.attrib = SEG_ATTR_CODE64;
    v->save.cs.limit = 0xFFFFFFFF; v->save.cs.base = 0;
    v->save.ss.selector = hd.ss; v->save.ss.attrib = SEG_ATTR_DATA;
    v->save.ss.limit = 0xFFFFFFFF; v->save.ss.base = 0;
    v->save.ds.selector = hd.ds; v->save.ds.attrib = SEG_ATTR_DATA;
    v->save.ds.limit = 0xFFFFFFFF; v->save.ds.base = 0;
    v->save.es.selector = hd.es; v->save.es.attrib = SEG_ATTR_DATA;
    v->save.es.limit = 0xFFFFFFFF; v->save.es.base = 0;
    v->save.gdtr.base = hd.gdt_base; v->save.gdtr.limit = hd.gdt_limit;
    v->save.idtr.base = hd.idt_base; v->save.idtr.limit = hd.idt_limit;
    v->save.cr0 = __readcr0();
    v->save.cr3 = __readcr3();
    v->save.cr4 = __readcr4();
    v->save.efer = __readmsr(MSR_EFER);
    v->save.cpl = 0;
    v->save.g_pat = 0x0007040600070406ull;
}

typedef UINT64 (*hv_resident_fn)(UINT64, UINT64, guest_gprs *, UINT64);

// M10.2: Copy our loaded image into EfiRuntimeServicesData (memory an OS keeps
// across ExitBootServices) and apply PE base relocations so the copy is fully
// self-contained. Outputs delta = (copy base - original base). We then run the
// resident hypervisor from the copy. Returns FALSE on failure.
static BOOLEAN relocate_hv_image(EFI_HANDLE image, EFI_BOOT_SERVICES *bs,
                                 UINT64 *out_delta) {
    static EFI_GUID li_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *li = 0;
    if (EFI_ERROR(bs->HandleProtocol(image, &li_guid, (VOID **)&li)) || !li)
        return FALSE;

    UINT64 old_base = (UINT64)li->ImageBase;
    UINT64 size     = li->ImageSize;
    UINT64 new_base = 0;
    UINTN  pages    = (UINTN)((size + 4095) / 4096);
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData,
                                    pages, &new_base)))
        return FALSE;

    memcpy((VOID *)new_base, (VOID *)old_base, (UINTN)size);
    UINT64 delta = new_base - old_base;

    // Walk the PE base-relocation table and fix up DIR64 entries in the copy.
    UINT8 *b        = (UINT8 *)old_base;
    UINT32 e_lfanew = *(UINT32 *)(b + 0x3C);
    UINT8 *opt      = b + e_lfanew + 24;             // OptionalHeader (PE32+)
    UINT32 reloc_rva  = *(UINT32 *)(opt + 152);      // DataDirectory[5] (BaseReloc)
    UINT32 reloc_size = *(UINT32 *)(opt + 156);
    UINT8 *r   = (UINT8 *)(new_base + reloc_rva);
    UINT8 *end = r + reloc_size;
    while (reloc_rva && reloc_size && r < end) {
        UINT32 page_rva   = *(UINT32 *)(r + 0);
        UINT32 block_size = *(UINT32 *)(r + 4);
        if (block_size < 8) break;
        UINT16 *e = (UINT16 *)(r + 8);
        UINT32  n = (block_size - 8) / 2;
        for (UINT32 i = 0; i < n; i++)
            if ((e[i] >> 12) == 10)                  // IMAGE_REL_BASED_DIR64
                *(UINT64 *)(new_base + page_rva + (e[i] & 0xFFF)) += delta;
        r += block_size;
    }
    *out_delta = delta;
    return TRUE;
}

void svm_go_resident(EFI_HANDLE image, EFI_BOOT_SERVICES *bs) {
    // Referenced by the resident hypervisor forever. VMCBs / page tables / stack
    // are EfiRuntimeServicesData allocations (persist); the GPR context lives in
    // the image and is reached through its relocated copy (below).
    static guest_gprs g_ctx;

    UINT64 guest_pa = alloc_page(bs);
    UINT64 host_pa  = alloc_page(bs);
    UINT64 hstack   = 0;
    if (!guest_pa || !host_pa ||
        EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 8, &hstack)))
        return;
    UINT64 hstack_top = hstack + 8 * 4096;

    vmcb *v = (vmcb *)guest_pa;
    fill_current_cpu_state(v);
    v->control.guest_asid     = 1;
    v->control.intercept_vec3 = INTERCEPT_CPUID;
    v->control.intercept_vec4 = INTERCEPT_VMRUN | INTERCEPT_VMMCALL;

    UINT64 npt = npt_build(bs, 0, 0);          // identity NPT: guest sees all RAM
    if (!npt) return;
    v->control.np_enable = 1;
    v->control.n_cr3     = npt;
    v->save.g_pat        = 0x0007040600070406ull;

    UINT64 hostpt = hostpt_build(bs);          // M10.1: host's own page tables
    if (!hostpt) return;

    // M10.2: relocate the hypervisor into persistent memory and target the
    // resident entry point + GPR context in the copy (add the relocation delta).
    UINT64 delta = 0;
    if (!relocate_hv_image(image, bs, &delta)) {
        print(L"[M10] FAIL: could not relocate hypervisor image.\r\n");
        return;
    }
    print_line(L"[M10] hypervisor relocated to persistent memory; delta = ", delta);

    hv_resident_fn fn  = (hv_resident_fn)((UINT64)&hv_resident + delta);
    guest_gprs    *gc  = (guest_gprs *)((UINT64)&g_ctx + delta);

    print(L"[M10] switching host page tables and going resident (from copy)...\r\n");
    __writecr3(hostpt);
    dbg_puts("[M10] resident hypervisor now running from relocated copy\r\n");

    fn(guest_pa, host_pa, gc, hstack_top);     // enter: returns as a guest
}
