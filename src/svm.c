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
#define INTERCEPT_MSR     (1u << 28)  // vector 3: intercept MSR access (per MSRPM)
#define INTERCEPT_VMRUN   (1u << 0)   // vector 4: required for VMRUN to be legal
#define INTERCEPT_VMMCALL (1u << 1)   // vector 4: intercept VMMCALL
#define VMEXIT_DB         0x041ull     // #DB (debug) exception - used for single-step
#define VMEXIT_NMI        0x061ull     // NMI - our edge-triggered AP wake signal
#define VMEXIT_INIT       0x063ull     // INIT signal (OS starting this core)
#define INTERCEPT_NMI     (1u << 1)    // vector-3 bit: intercept NMI
#define VMEXIT_CPUID      0x072ull
#define VMEXIT_MSR        0x07Cull     // RDMSR/WRMSR of an intercepted MSR
#define EXC_DB            (1u << 1)    // intercept_exceptions bit for vector 1 (#DB)
#define RFLAGS_TF         (1ull << 8)  // trap flag: #DB after each instruction
#define RFLAGS_IF         (1ull << 9)  // interrupt flag
#define VMEXIT_VMMCALL    0x081ull
#define VMEXIT_NPF        0x400ull     // nested page fault (guest-physical)
#define VMEXIT_INVALID    (~0ull)     // guest state failed consistency checks

// x2APIC Interrupt Command Register (single MSR; a write sends an IPI). We watch
// this to see the OS start its APs via INIT/SIPI. Delivery mode is bits [10:8]:
// 5 = INIT, 6 = Startup (SIPI); vector (SIPI trampoline page) is bits [7:0];
// destination x2APIC ID is bits [63:32].
#define MSR_X2APIC_ICR    0x830

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

// ---- Guest tools hypercall ABI --------------------------------------------
// A program running INSIDE the guest (e.g. minictl.exe) talks to the hypervisor
// by putting HV_MAGIC in RAX and a command in RCX (and an optional argument in
// RDX), then executing VMMCALL. We answer in RAX. VMMCALL is already intercepted
// and the OS never uses it, so this channel is free.
#define HV_MAGIC          0x4D696E69534D31ull   // "1MSiniM" - our handshake key
#define HV_REPLY          0xC0FFEEC0FFEEull     // ping reply -> "I'm here"
#define HVC_PING          0                     // -> HV_REPLY
#define HVC_EXITS         1                     // -> total #VMEXITs serviced
#define HVC_WINVER        2                     // -> Windows major version (VMI)
#define HVC_VERSION       3                     // -> hypervisor version
#define HVC_FIND_PID      4                     // RDX=ptr to name -> PID (0=none)
// SMP.3b diagnostics - returned through RAX because the hypervisor's serial
// writes are swallowed once Windows owns COM1, but the hypercall channel works.
#define HVC_APICBASE      5                     // -> live IA32_APIC_BASE (bit10=x2apic)
#define HVC_SMP_COUNT     6                     // -> # INIT/SIPI ICR writes intercepted
#define HVC_SMP_SIPI      7                     // -> last SIPI ICR value (low byte=vector)
// SMP.3b xAPIC MMIO emulator, increment 1 (observe the first APIC-page write):
#define HVC_APIC_FAULTS   8                     // -> # APIC-page NPT write-faults seen
#define HVC_APIC_RIP      9                     // -> guest RIP of the first APIC write
#define HVC_APIC_B0       10                    // -> first 8 instruction bytes
#define HVC_APIC_B1       11                    // -> next 8 instruction bytes
#define HVC_APIC_GPA      12                    // -> first faulting GPA (low12 = register)
#define HVC_APIC_INFO1    13                    // -> NPF exit_info1 (bit1 = write)
#define HVC_DB_COUNT      14                    // -> # completed single-steps (#DB)
#define HVC_ICR_LO        15                    // -> last ICR-low value (mode+vector)
#define HVC_ICR_HI        16                    // -> last ICR-high value (destination)
#define HVC_ICR_COUNT     17                    // -> # ICR sends (IPIs)
#define HVC_INIT_CNT      18                    // -> # INIT IPIs decoded
#define HVC_SIPI_CNT      19                    // -> # SIPI IPIs decoded
#define HVC_SIPI_VEC      20                    // -> SIPI trampoline vector (page<<12)
#define HVC_APIC_UNDEC    21                    // -> # ICR writes we couldn't decode
#define HVC_AP_STARTED    22                    // -> # APs that ran their trampoline
#define HVC_AP_BADEXIT    23                    // -> exit_code of last AP unexpected exit
#define HVC_AP_BADRIP     24                    // -> guest RIP at that exit
#define HVC_AP_BADCOUNT   25                    // -> # unexpected exits (AP deaths)
#define HVC_RM_EXIT       26                    // -> exit_code an AP loops on in real mode
#define HVC_RM_RIP        27                    // -> guest RIP at that exit
#define HVC_INIT_LOOPS    28                    // -> max per-AP INIT-handler re-entries

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

static UINT64 g_win_major = 0;   // Windows major version, captured by VMI below

// SMP.3b: what the x2APIC ICR intercept has observed (exposed via hypercall).
static volatile UINT64 g_ipi_seen  = 0;   // count of INIT/SIPI ICR writes seen
static volatile UINT64 g_last_sipi = 0;   // last SIPI ICR value (0 = none yet)

// SMP.3b xAPIC MMIO emulator: the local-APIC page is write-protected in the BSP's
// NPT; the first write faults, we snapshot the instruction, then un-protect so
// Windows continues (increment 1 = observe). g_apic_pte points at the 4 KiB NPT
// PTE for 0xFEE00000 so we can flip its writable bit at runtime.
#define APIC_PAGE_GPA  0xFEE00000ull
#define NPT_PTE_RW     (1ull << 1)
// SMP.3b AP-startup emulation (xAPIC MMIO trap -> swallow INIT/SIPI -> NMI-wake ->
// real-mode reset into Windows' trampoline). It WORKS up to the point of getting
// the AP to run Windows' AP-startup code, but under nested VMware the AP can't
// complete Windows' AP-init cleanly (KERNEL_SECURITY_CHECK_FAILURE + monitor
// panic). Left OFF by default so the APs start natively (a working multi-core
// Windows, with the APs virtualized by the layer below us). Flip to 1 to enable
// the emulator - most likely to succeed on BARE METAL, where x2APIC/AVIC and
// non-nested SVM remove these limits.
#define SMP_EMULATE_AP_STARTUP  0
static UINT64 *g_apic_pte      = 0;
static volatile UINT64 g_apic_faults = 0;
static volatile UINT64 g_apic_rip    = 0;
static volatile UINT64 g_apic_b0     = 0;
static volatile UINT64 g_apic_b1     = 0;
static volatile UINT64 g_apic_gpa    = 0;
static volatile UINT64 g_apic_info1  = 0;
// Increment 2: single-step machinery. We keep the APIC page trapped and step each
// write through under TF for a bounded budget, then un-protect for speed.
static UINT64  g_ss_budget = 100000;   // cap; we stop early once the SIPI is caught
static BOOLEAN g_ss_active = FALSE;
static UINT64  g_ss_if     = 0;   // guest IF saved across the single-step
static volatile UINT64 g_db_count = 0;   // # completed single-steps (proof it works)
// Increment 3: what we decode out of the ICR writes (the AP-startup IPIs).
#define APIC_ICR_LO 0x300     // write here sends the IPI; bits8-10=mode, 0-7=vector
#define APIC_ICR_HI 0x310     // destination (xAPIC ID in bits 24-31)
static volatile UINT64 g_icr_lo = 0, g_icr_hi = 0;
static volatile UINT64 g_icr_count = 0, g_init_cnt = 0, g_sipi_cnt = 0, g_sipi_vec = 0;
static volatile UINT64 g_undec = 0;   // ICR writes we couldn't decode
static UINT64 g_last_ipi_fault = 0;   // APIC-fault count at the last INIT/SIPI

// Increment 4: cross-core SIPI signaling. The BSP and APs run from SEPARATE
// relocated image copies, so they cannot share ordinary globals - instead they
// share this page, allocated once at UEFI time with its address captured BEFORE
// any relocation (so every copy holds the same physical pointer). Indexed by
// APIC ID: the BSP writes the SIPI vector here; the parked AP polls for it.
typedef struct {
    volatile UINT32 sipi_vec[64];    // BSP -> AP: (vector | 0x100) means "start here"
    volatile UINT32 ap_started[64];  // AP -> BSP: set when the AP runs its trampoline
    volatile UINT64 last_bad_exit;   // AP diag: exit_code of the last unexpected exit
    volatile UINT64 last_bad_rip;    // AP diag: guest RIP at that exit
    volatile UINT64 bad_count;       // AP diag: how many unexpected exits total
    volatile UINT64 rm_exit;         // AP diag: exit_code while the guest is real-mode
    volatile UINT64 rm_rip;          // AP diag: guest RIP at that real-mode exit
    volatile UINT32 init_loops[64];  // per-AP: times the wake (NMI) handler ran
    volatile UINT32 nmi_sent[64];    // per-AP: BSP has already NMI-woken this core
} smp_shared_t;
static smp_shared_t *g_smp = 0;      // same physical page in every image copy

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
        g_win_major = *maj;
        dbg_hex("[VMI]   NtMajorVersion : ", *maj);
        dbg_hex("[VMI]   NtMinorVersion : ", *min);
    }
    dbg_hex("[VMI]   (walked via guest CR3 = ", v->save.cr3);
    dbg_puts("[VMI] === we just read that out of Windows' own address space ===\r\n\r\n");
}

// ---- VMI: find a process by name ------------------------------------------
//
// This is the showcase: from BENEATH Windows, with no driver and no help from
// the guest, we walk the kernel's own list of running processes and find one by
// name. It's the same primitive an EDR/anti-cheat/forensics tool uses - except
// we're doing it from the hypervisor.
//
// How: every EPROCESS (the kernel's process object) is threaded onto a circular
// doubly-linked list via its ActiveProcessLinks field. We get a first EPROCESS
// from the current thread (KPCR -> KPRCB.CurrentThread -> KTHREAD.Process), then
// walk the ring, reading each process's ImageFileName and comparing to the name.
//
// These are the field offsets for 64-bit Windows 10/11 (19041+). They are
// build-specific; if a future build moves them, the walk simply finds nothing
// (every read is bounds-checked, so a wrong offset can't crash the host).
//
// NOTE: this reads *kernel* memory using the calling process's CR3. That works
// because AMD CPUs aren't affected by Meltdown, so Windows leaves KPTI/KVA-shadow
// OFF - the full kernel stays mapped in every process. (On an Intel host with
// KPTI on, we'd instead need the kernel CR3.)
#define OFF_KPCR_CURRENT_THREAD  0x188   // KPCR.Prcb(0x180).CurrentThread(0x08)
#define OFF_KTHREAD_PROCESS      0x0B8   // KTHREAD.ApcState(0x98).Process(0x20)
#define OFF_EPROC_UNIQUE_PID     0x440   // EPROCESS.UniqueProcessId
#define OFF_EPROC_LINKS          0x448   // EPROCESS.ActiveProcessLinks (LIST_ENTRY)
#define OFF_EPROC_IMAGENAME      0x5A8   // EPROCESS.ImageFileName (15 chars + NUL)

// Case-insensitive compare of up to n bytes; stops at a NUL in either string.
static BOOLEAN name_ieq(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return FALSE;
        if (ca == 0)  return TRUE;
    }
    return TRUE;
}

// Read a UINT64 out of guest kernel memory, or return 0 if not mapped.
static UINT64 guest_rd64(vmcb *v, UINT64 gva) {
    UINT64 *p = (UINT64 *)guest_va_to_host(v, gva);
    return p ? *p : 0;
}

// Walk the active-process list and return the PID whose image name matches
// `want` (e.g. "notepad.exe"), or 0 if none / offsets don't match this build.
static UINT64 vmi_find_pid(vmcb *v, const char *want) {
    // KernelGsBase holds the KPCR while the guest is in user mode (which it is
    // when minictl issues its VMMCALL). From there: current thread -> process.
    UINT64 kpcr = v->save.kernel_gs_base;
    if (!kpcr) return 0;
    UINT64 ethread = guest_rd64(v, kpcr + OFF_KPCR_CURRENT_THREAD);
    if (!ethread) return 0;
    UINT64 eproc = guest_rd64(v, ethread + OFF_KTHREAD_PROCESS);
    if (!eproc) return 0;

    UINT64 start = eproc;
    for (int i = 0; i < 4096 && eproc; i++) {       // bounded: never loop forever
        char *img = (char *)guest_va_to_host(v, eproc + OFF_EPROC_IMAGENAME);
        if (img && name_ieq(img, want, 15))
            return guest_rd64(v, eproc + OFF_EPROC_UNIQUE_PID);
        // Follow ActiveProcessLinks.Flink to the next EPROCESS. The link points
        // at the *middle* of the next EPROCESS, so subtract the field offset.
        UINT64 flink = guest_rd64(v, eproc + OFF_EPROC_LINKS);
        if (!flink) break;
        eproc = flink - OFF_EPROC_LINKS;
        if (eproc == start) break;                  // came full circle
    }
    return 0;
}

// Read guest general register `n` (0=RAX .. 15=R15) as 64-bit. RAX/RSP live in
// the VMCB save area; the rest are in the saved GPR block (order per vmrun.asm).
static UINT64 guest_reg(vmcb *v, guest_gprs *g, int n) {
    switch (n) {
        case 0:  return v->save.rax;   case 1:  return g->rcx;
        case 2:  return g->rdx;        case 3:  return g->rbx;
        case 4:  return v->save.rsp;   case 5:  return g->rbp;
        case 6:  return g->rsi;        case 7:  return g->rdi;
        case 8:  return g->r8;         case 9:  return g->r9;
        case 10: return g->r10;        case 11: return g->r11;
        case 12: return g->r12;        case 13: return g->r13;
        case 14: return g->r14;        default: return g->r15;
    }
}

// Best-effort decode of the 32-bit value a MOV-to-memory APIC store writes, and
// its total byte length (so the caller can skip/"swallow" the instruction).
// Windows' HAL uses `mov [reg+disp], r32` (opcode 0x89, value in the ModRM.reg
// register - confirmed by increment 1) and sometimes `mov [mem], imm32` (0xC7).
// Returns FALSE for any other form (caller counts it and passes through anyway).
static BOOLEAN decode_apic_write(vmcb *v, guest_gprs *g, UINT32 *out, int *len) {
    UINT8 *ip = (UINT8 *)guest_va_to_host(v, v->save.rip);
    if (!ip) return FALSE;
    int i = 0;
    UINT8 rex = 0;
    for (;;) {                                  // skip prefixes, keep REX
        UINT8 b = ip[i];
        if (b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 || b == 0xF3 ||
            b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26 || b == 0x64 || b == 0x65) {
            i++; continue;
        }
        if (b >= 0x40 && b <= 0x4F) { rex = b; i++; continue; }
        break;
    }
    UINT8 op = ip[i++];
    UINT8 modrm = ip[i++];
    int mod = modrm >> 6, reg = (modrm >> 3) & 7, rm = modrm & 7;
    if (op != 0x89 && op != 0xC7) return FALSE;
    if (op == 0x89)                             // MOV r/m32, r32 -> value in reg
        *out = (UINT32)guest_reg(v, g, reg | ((rex & 0x4) ? 8 : 0));   // + REX.R
    // Walk past ModRM's SIB/displacement to find the instruction end.
    if (mod != 3) {
        if (rm == 4) i++;                                       // SIB byte
        if (mod == 1) i += 1;                                   // disp8
        else if (mod == 2) i += 4;                              // disp32
        else if (mod == 0 && rm == 5) i += 4;                  // RIP-relative disp32
    }
    if (op == 0xC7) {                           // MOV r/m32, imm32 -> value = imm
        *out = (UINT32)(ip[i] | (ip[i+1] << 8) | (ip[i+2] << 16) | ((UINT32)ip[i+3] << 24));
        i += 4;
    }
    if (len) *len = i;
    return TRUE;
}

// Send an NMI IPI to the given (physical, xAPIC) APIC ID via this core's local
// APIC. The APIC MMIO region is UC by MTRR, so a plain write reaches it. Used to
// wake a parked AP for startup without a (latching) INIT.
static void send_nmi(UINT32 apicid) {
    volatile UINT32 *icr_hi = (volatile UINT32 *)(APIC_PAGE_GPA + 0x310);
    volatile UINT32 *icr_lo = (volatile UINT32 *)(APIC_PAGE_GPA + 0x300);
    *icr_hi = apicid << 24;
    *icr_lo = 0x00004400;    // delivery mode 4 (NMI), assert, edge, dest = ICR-high
}

// Increment 4: set an AP's guest VMCB to the state a real CPU has right after
// receiving SIPI with the given `vector`: 16-bit real mode, CS = vector<<8 with
// base vector<<12, everything else at INIT/reset defaults. VMRUN then runs
// Windows' real-mode AP trampoline at that address as our guest, and it will
// bring itself up to long mode and into the AP kernel entry - staying our guest.
static void set_realmode_ap(vmcb *v, guest_gprs *g, UINT32 vector) {
    v->save.cs.selector = (UINT16)(vector << 8);
    v->save.cs.base     = (UINT64)vector << 12;
    v->save.cs.limit    = 0xFFFF;
    v->save.cs.attrib   = 0x009B;                 // 16-bit code, r/x, present
    v->save.ss.selector = 0; v->save.ss.base = 0; v->save.ss.limit = 0xFFFF;
    v->save.ss.attrib   = 0x0093;                 // 16-bit data, r/w, present
    v->save.ds = v->save.es = v->save.fs = v->save.gs = v->save.ss;  // all base 0
    v->save.gdtr.base = 0; v->save.gdtr.limit = 0xFFFF;
    v->save.idtr.base = 0; v->save.idtr.limit = 0xFFFF;
    v->save.ldtr.selector = 0; v->save.ldtr.base = 0; v->save.ldtr.limit = 0xFFFF;
    v->save.ldtr.attrib = 0x0082;
    v->save.tr.selector = 0; v->save.tr.base = 0; v->save.tr.limit = 0xFFFF;
    v->save.tr.attrib = 0x008B;
    v->save.cr0   = 0x60000010;                   // CD|NW|ET, PE=0, PG=0 (real mode)
    v->save.cr2   = 0; v->save.cr3 = 0; v->save.cr4 = 0;
    v->save.efer  = 0x1000;                        // SVME only (required for VMRUN)
    v->save.rip   = 0;
    v->save.rsp   = 0;
    v->save.rflags = 0x2;
    v->save.rax   = 0;
    v->save.dr6   = 0xFFFF0FF0; v->save.dr7 = 0x400;
    v->save.cpl   = 0;
    v->save.g_pat = 0x0007040600070406ull;
    // Reset the general-purpose registers (RDX = a plausible family/model, as a
    // real CPU leaves it after reset; the rest zero).
    g->rbx = g->rcx = 0; g->rdx = 0x600;
    g->rsi = g->rdi = g->rbp = 0;
    g->r8 = g->r9 = g->r10 = g->r11 = 0;
    g->r12 = g->r13 = g->r14 = g->r15 = 0;
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
    // Default: no TLB flush. A handler that changes an NPT permission sets
    // tlb_control=1 to flush once on the next VMRUN; clearing it here means that
    // flush happens exactly once instead of on every subsequent VMRUN.
    v->control.tlb_control = 0;
    // Diagnostic: capture what a real-mode guest (an AP running its trampoline)
    // is exiting on. The BSP is always in long mode (PE=1), so this only tracks
    // the APs.
    if (g_smp && !(v->save.cr0 & 1)) {
        g_smp->rm_exit = v->control.exit_code;
        g_smp->rm_rip  = v->save.rip;
    }
    // Only log guest RIP for the long-mode BSP - a looping real-mode AP would
    // otherwise flood the serial.
    if ((v->save.cr0 & 1) && (exits <= 32 || (exits & 0x1FFF) == 0))
        dbg_hex("[hv] guest rip=", v->save.rip);
    // Milestone: the guest RIP entering the high canonical half means the
    // Windows KERNEL (ntoskrnl) is now executing as our guest. At that point we
    // have proven virtualization, so DISABLE CPUID interception (clean_bits are
    // 0, so the CPU reloads intercepts on the next VMRUN) - the guest then runs
    // at near-native speed and can boot to the desktop. We stay resident (VMRUN
    // intercept + NPT), so Windows is still our guest, just faster.
    static BOOLEAN vmi_done = FALSE;
    static UINT64 kexits = 0;
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
                vmi_done = TRUE;
            } else if (kexits > 40000) {
                dbg_puts("[hv] (VMI: kernel data not readable; skipping)\r\n");
                vmi_done = TRUE;
            } else if ((kexits & 0x7FF) == 0) {   // heartbeat so it's never silent
                dbg_hex("[hv] VMI waiting for kernel to map its data; cr3=", v->save.cr3);
            }
            if (vmi_done) {
                dbg_puts("[hv] disabling CPUID intercept -> guest now runs at native speed.\r\n");
                // Clear ONLY CPUID - keep any other vec3 intercepts (e.g. the
                // MSR intercept we use to watch AP startup) active.
                v->control.intercept_vec3 &= ~INTERCEPT_CPUID;
            }
        }
    }

    UINT64 code = v->control.exit_code;

    if (code == VMEXIT_DB) {
        // End of a single-step (increment 2): the APIC write we allowed has now
        // executed (RIP already points past it). Re-protect the APIC page, drop
        // the #DB intercept, and restore the guest's trap/interrupt flags.
        if (g_ss_active) {
            g_ss_active = FALSE;
            g_db_count++;
            if (g_apic_pte) *g_apic_pte &= ~NPT_PTE_RW;
            v->control.tlb_control = 1;
            v->control.intercept_exceptions &= ~EXC_DB;
            v->save.rflags = (v->save.rflags & ~RFLAGS_TF) | g_ss_if;
        }
        return;
    }

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
        // Guest tools channel: RAX == HV_MAGIC means a program inside the guest
        // (e.g. minictl.exe) is calling us. The command is in RCX; we answer in
        // RAX. Works from any privilege level, since VMMCALL traps to us.
        //
        // DIAGNOSTIC: log the first handful of VMMCALLs we see (guest RAX/RCX),
        // so if minictl's hypercall misbehaves we can tell whether it even
        // reaches us and with what register values. Windows never issues
        // VMMCALL, so this only fires for our own tool.
        {
            static UINT64 vmc = 0;
            if (vmc < 24) {
                vmc++;
                if (vmc == 1)
                    // Live APIC mode on this core: bit 11 = APIC enabled,
                    // bit 10 = x2APIC enabled. Tells us definitively whether
                    // Windows switched to x2APIC (so our 0x830 intercept can fire)
                    // or is still on legacy xAPIC (MMIO).
                    dbg_hex("[hv] IA32_APIC_BASE (bit10=x2apic,bit11=en)=",
                            __readmsr(0x1Bu));
                dbg_hex("[hv] VMMCALL guest rax=", v->save.rax);
                dbg_hex("[hv]         guest rcx=", g->rcx);
            }
        }
        if (v->save.rax == HV_MAGIC) {
            switch (g->rcx) {
                case HVC_PING:    v->save.rax = HV_REPLY;      break;
                case HVC_EXITS:   v->save.rax = exits;         break;
                case HVC_WINVER:  v->save.rax = g_win_major;   break;
                case HVC_VERSION: v->save.rax = 0x00010000;    break;  // v1.0
                case HVC_APICBASE:  v->save.rax = __readmsr(0x1Bu); break;
                case HVC_SMP_COUNT: v->save.rax = g_ipi_seen;       break;
                case HVC_SMP_SIPI:  v->save.rax = g_last_sipi;      break;
                case HVC_APIC_FAULTS: v->save.rax = g_apic_faults;  break;
                case HVC_APIC_RIP:    v->save.rax = g_apic_rip;     break;
                case HVC_APIC_B0:     v->save.rax = g_apic_b0;      break;
                case HVC_APIC_B1:     v->save.rax = g_apic_b1;      break;
                case HVC_APIC_GPA:    v->save.rax = g_apic_gpa;     break;
                case HVC_APIC_INFO1:  v->save.rax = g_apic_info1;   break;
                case HVC_DB_COUNT:    v->save.rax = g_db_count;     break;
                case HVC_ICR_LO:      v->save.rax = g_icr_lo;       break;
                case HVC_ICR_HI:      v->save.rax = g_icr_hi;       break;
                case HVC_ICR_COUNT:   v->save.rax = g_icr_count;    break;
                case HVC_INIT_CNT:    v->save.rax = g_init_cnt;     break;
                case HVC_SIPI_CNT:    v->save.rax = g_sipi_cnt;     break;
                case HVC_SIPI_VEC:    v->save.rax = g_sipi_vec;     break;
                case HVC_APIC_UNDEC:  v->save.rax = g_undec;        break;
                case HVC_AP_STARTED: {
                    UINT64 n = 0;
                    if (g_smp) for (int k = 0; k < 64; k++) if (g_smp->ap_started[k]) n++;
                    v->save.rax = n;
                    break;
                }
                case HVC_AP_BADEXIT:  v->save.rax = g_smp ? g_smp->last_bad_exit : 0; break;
                case HVC_AP_BADRIP:   v->save.rax = g_smp ? g_smp->last_bad_rip  : 0; break;
                case HVC_AP_BADCOUNT: v->save.rax = g_smp ? g_smp->bad_count     : 0; break;
                case HVC_RM_EXIT:     v->save.rax = g_smp ? g_smp->rm_exit : 0; break;
                case HVC_RM_RIP:      v->save.rax = g_smp ? g_smp->rm_rip  : 0; break;
                case HVC_INIT_LOOPS: {
                    UINT64 mx = 0;
                    if (g_smp) for (int k = 0; k < 64; k++)
                        if (g_smp->init_loops[k] > mx) mx = g_smp->init_loops[k];
                    v->save.rax = mx;
                    break;
                }
                case HVC_FIND_PID: {
                    // RDX = guest pointer to a NUL-terminated process name.
                    // Copy it into a small local buffer, then walk the kernel's
                    // process list for it and return the PID (0 = not found).
                    char want[16]; int k = 0;
                    char *nm = (char *)guest_va_to_host(v, g->rdx);
                    if (nm) while (k < 15 && nm[k]) { want[k] = nm[k]; k++; }
                    want[k] = 0;
                    v->save.rax = nm ? vmi_find_pid(v, want) : 0;
                    break;
                }
                default:          v->save.rax = 0;             break;
            }
            dbg_hex("[hv]         -> reply rax=", v->save.rax);
        }
        v->save.rip = v->control.nrip ? v->control.nrip
                                      : v->save.rip + VMMCALL_LEN;
        return;
    }

    if (code == VMEXIT_MSR) {
        // We only intercept WRITES to the x2APIC ICR (0x830). Decode the IPI and
        // log INIT/SIPI (SMP.3b step 1: observe-only), then forward the write so
        // AP startup is unchanged for now. Other MSR accesses shouldn't reach
        // here (nothing else is set in the MSRPM), but we pass them through to be
        // safe rather than swallow them.
        UINT32  msr   = (UINT32)g->rcx;
        BOOLEAN write = (v->control.exit_info1 & 1) != 0;
        UINT64  val   = ((UINT64)(UINT32)g->rdx << 32) | (UINT32)v->save.rax;
        if (write && msr == MSR_X2APIC_ICR) {
            UINT32 dm = (UINT32)((val >> 8) & 7);           // delivery mode
            if (dm == 5 || dm == 6) {                       // INIT / SIPI
                g_ipi_seen++;                               // read back via hypercall
                if (dm == 6) g_last_sipi = val;             // SIPI carries the vector
                dbg_puts(dm == 5 ? "[SMP] BSP -> INIT  " : "[SMP] BSP -> SIPI  ");
                dbg_hex("x2apic dest=", val >> 32);
                dbg_hex("           ICR=", val);
                // Once AP startup is done, stop trapping the ICR - otherwise
                // every TLB-shootdown / scheduler IPI would keep trapping and
                // crawl the system. (~3 IPIs per AP started.)
                if (g_ipi_seen >= 24) {
                    v->control.intercept_vec3 &= ~INTERCEPT_MSR;
                    dbg_puts("[SMP] AP startup observed; ICR intercept off.\r\n");
                }
            }
            __writemsr(MSR_X2APIC_ICR, val);                // forward unchanged
        } else if (write) {
            __writemsr(msr, val);
        } else {
            UINT64 r = __readmsr(msr);
            v->save.rax = (UINT32)r;
            g->rdx      = (UINT32)(r >> 32);
        }
        v->save.rip = v->control.nrip ? v->control.nrip : v->save.rip + 2;
        return;
    }

    if (code == VMEXIT_NMI) {
        // Increment 7: the BSP woke this parked AP with an NMI (instead of a
        // latching INIT) because the OS wants to start it. Read our published
        // trampoline vector and reset the guest into real mode there, so it runs
        // Windows' AP-startup code as our guest. NMI is edge-triggered, so this
        // fires exactly once - and we then drop the NMI intercept so any later
        // (genuine) NMIs go to Windows normally.
        int r[4];
        __cpuidex(r, 1, 0);
        UINT32 apicid = (UINT32)((r[1] >> 24) & 0x3F);   // CPUID.1:EBX[31:24]
        if (g_smp && apicid < 64) g_smp->init_loops[apicid]++;
        if (g_smp && apicid < 64 && (g_smp->sipi_vec[apicid] & 0x100)) {
            UINT32 vector = g_smp->sipi_vec[apicid] & 0xFF;
            set_realmode_ap(v, g, vector);                // reset guest -> real mode
            v->control.tlb_control = 1;                    // flush stale translations
            v->control.intercept_vec3 &= ~INTERCEPT_NMI;  // one-shot: stop trapping NMI
            g_smp->ap_started[apicid] = 1;                // observable via hypercall
            return;                                       // VMRUN runs the trampoline
        }
        // A stray NMI with no pending start - just resume.
        v->control.intercept_vec3 &= ~INTERCEPT_NMI;
        return;
    }

    if (code == VMEXIT_NPF && (v->control.exit_info2 & ~0xFFFull) == APIC_PAGE_GPA) {
        // SMP.3b increment 1: the guest wrote the local-APIC MMIO page (which we
        // write-protected). Snapshot the FIRST such access - the faulting GPA
        // (its low 12 bits are the APIC register offset), the guest RIP, and the
        // instruction bytes - so we can design the write decoder. Then un-protect
        // the page and re-run the instruction so Windows carries on normally.
        if (g_apic_faults == 0) {
            g_apic_gpa   = v->control.exit_info2;
            g_apic_rip   = v->save.rip;
            g_apic_info1 = v->control.exit_info1;
            UINT8 *ip = (UINT8 *)guest_va_to_host(v, v->save.rip);
            if (ip) {
                UINT64 b0 = 0, b1 = 0;
                for (int i = 0; i < 8; i++) b0 |= (UINT64)ip[i]     << (i * 8);
                for (int i = 0; i < 8; i++) b1 |= (UINT64)ip[i + 8] << (i * 8);
                g_apic_b0 = b0;
                g_apic_b1 = b1;
            }
        }
        g_apic_faults++;

        // Increment 3: capture the ICR writes (the AP-startup IPIs). 0x310 holds
        // the destination; 0x300 sends it with delivery mode in bits 8-10
        // (5=INIT, 6=SIPI) and, for SIPI, the trampoline vector in bits 0-7.
        UINT32 off = (UINT32)(v->control.exit_info2 & 0xFFF);
        if (off == APIC_ICR_LO || off == APIC_ICR_HI) {
            UINT32 val = 0; int ilen = 0;
            if (decode_apic_write(v, g, &val, &ilen)) {
                if (off == APIC_ICR_HI) g_icr_hi = val;
                else {
                    g_icr_lo = val; g_icr_count++;
                    UINT32 dm = (val >> 8) & 7;
                    if (dm == 5) { g_init_cnt++; g_last_ipi_fault = g_apic_faults; }
                    if (dm == 6) {
                        g_sipi_cnt++; g_sipi_vec = val & 0xFF;
                        g_last_ipi_fault = g_apic_faults;
                        // Publish the trampoline vector for every AP (all identical).
                        if (g_smp)
                            for (int k = 0; k < 64; k++)
                                g_smp->sipi_vec[k] = (val & 0xFF) | 0x100;
                    }
                    // Increment 7: SWALLOW INIT (5) and SIPI (6) - do NOT let the
                    // physical IPI reach the AP (an intercepted INIT would latch
                    // and loop). Instead, skip the instruction, and for the SIPI
                    // wake the target AP with an NMI (edge-triggered, no latch);
                    // its NMI handler resets it into the trampoline as our guest.
                    if (dm == 5 || dm == 6) {
                        if (dm == 6) {
                            UINT32 dest = (UINT32)((g_icr_hi >> 24) & 0x3F);
                            // Exactly one NMI per AP (the OS sends SIPI twice).
                            if (g_smp && dest < 64 && !g_smp->nmi_sent[dest]) {
                                g_smp->nmi_sent[dest] = 1;
                                send_nmi(dest);
                            }
                        }
                        v->save.rip += ilen;         // swallow (skip the write)
                        return;
                    }
                }
            } else {
                g_undec++;
            }
        }

        // Stop trapping once the AP-startup burst is over (no new IPI for a while)
        // or the safety budget runs out - so Windows returns to full speed. The
        // burst stays trapped so we swallow+NMI every AP's INIT/SIPI.
        if ((g_sipi_cnt >= 1 && g_apic_faults - g_last_ipi_fault > 3000) ||
            g_ss_budget == 0) {
            if (g_apic_pte) *g_apic_pte |= NPT_PTE_RW;   // done: let Windows run free
            v->control.tlb_control = 1;
            return;
        }
        g_ss_budget--;
        g_ss_active = TRUE;
        if (g_apic_pte) *g_apic_pte |= NPT_PTE_RW;       // allow the faulting write
        v->control.tlb_control = 1;                      // flush stale RO mapping
        v->control.intercept_exceptions |= EXC_DB;       // trap after one instruction
        g_ss_if = v->save.rflags & RFLAGS_IF;            // run it with interrupts off
        v->save.rflags = (v->save.rflags & ~RFLAGS_IF) | RFLAGS_TF;
        return;                                           // re-run the write under TF
    }

    if (code == VMEXIT_NPF) {
        // An unexpected NPF. With a correct, persistent, identity NPT these
        // should not happen for mapped RAM - but if one does, log a few and
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

    // Truly unexpected exit: report via serial (dead post-boot) AND record it in
    // the shared page so an AP that dies after its real-mode reset is visible to
    // minictl on the BSP (e.g. a rejected real-mode VMCB shows exit_code == ~0).
    if (g_smp) {
        g_smp->last_bad_exit = code;
        g_smp->last_bad_rip  = v->save.rip;
        g_smp->bad_count++;
    }
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

    // SMP.3b step 1 (observe-only): intercept writes to the x2APIC ICR so we can
    // see the BSP start the APs via INIT/SIPI. The MSR permission map is 8 KiB;
    // an all-zero map intercepts nothing, so we set just the write-intercept bit
    // for MSR 0x830. We LOG INIT/SIPI and forward the write unchanged for now -
    // this confirms whether the OS uses x2APIC (MSR) or legacy xAPIC (MMIO).
    UINT64 msrpm = 0;
    if (!EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 2, &msrpm)) && msrpm) {
        UINT8 *m = (UINT8 *)msrpm;
        for (UINTN k = 0; k < 2 * 4096; k++) m[k] = 0;   // 0 = allow (no intercept)
        m[MSR_X2APIC_ICR / 4] |= (UINT8)(1u << ((MSR_X2APIC_ICR % 4) * 2 + 1));
        v->control.msrpm_base_pa   = msrpm;
        v->control.intercept_vec3 |= INTERCEPT_MSR;
    }

    UINT64 npt = npt_build(bs, 0, 0);          // identity NPT: guest sees all RAM
    if (!npt) return;
    v->control.np_enable = 1;
    v->control.n_cr3     = npt;
    v->save.g_pat        = 0x0007040600070406ull;

    // SMP.3b increment 1: write-protect the local-APIC MMIO page in the NPT so we
    // trap the guest's APIC register writes (including the ICR that starts the
    // APs). We only need to see the FIRST one to learn the instruction form, then
    // the #NPF handler un-protects and lets Windows continue.
    if (SMP_EMULATE_AP_STARTUP) {
        g_apic_pte = npt_pte_ptr(bs, npt, APIC_PAGE_GPA);
        if (g_apic_pte) *g_apic_pte &= ~NPT_PTE_RW;  // read-only -> writes fault
    }

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

// ---- SMP: virtualize the other CPU cores (M12) -----------------------------
//
// Each Application Processor gets its own VMCB, host VMCB, host stack and HSAVE
// (allocated by the BSP, since APs can't safely call boot services), plus a
// shared identity NPT and host page table. On the AP, svm_virtualize_ap()
// self-virtualizes that core exactly like svm_go_resident does for the BSP.
#define MAX_CPUS 64
#define INTERCEPT_INIT (1u << 3)   // vector-3 bit: intercept INIT (AP startup)
typedef struct {
    UINT64 guest_vmcb, host_vmcb, host_stack_top, hsave;
    guest_gprs gctx;
} ap_state_t;
static ap_state_t *g_ap[MAX_CPUS];              // per-core state (persistent heap)
static UINT64 g_ap_npt = 0, g_ap_hostpt = 0;
static UINT64 g_ap_delta = 0;                   // relocation delta for AP loops

// BSP: relocate a persistent copy of the hypervisor for the APs to run their
// resident loops from (so they survive the OS's ExitBootServices). Separate copy
// from the BSP's, so the two don't share the per-copy exit-handler statics.
BOOLEAN svm_relocate_aps(EFI_HANDLE image, EFI_BOOT_SERVICES *bs) {
    return relocate_hv_image(image, bs, &g_ap_delta);
}

// BSP: allocate the page the BSP and APs use to hand off SIPI vectors. MUST be
// called BEFORE any relocate (svm_relocate_aps / svm_go_resident) so that every
// relocated image copy captures the same g_smp pointer. Persistent memory.
BOOLEAN svm_smp_shared_init(EFI_BOOT_SERVICES *bs) {
    UINT64 p = alloc_page(bs);                 // zeroed EfiRuntimeServicesData page
    if (!p) return FALSE;
    g_smp = (smp_shared_t *)p;
    return TRUE;
}

// BSP: build the NPT + host page tables shared by every AP (identity, so one
// copy is fine for all of them). Call once before starting the APs.
BOOLEAN svm_build_ap_tables(EFI_BOOT_SERVICES *bs) {
    g_ap_npt    = npt_build(bs, 0, 0);
    g_ap_hostpt = hostpt_build(bs);
    return g_ap_npt && g_ap_hostpt;
}

// BSP: pre-allocate the per-core state for AP slot `i` (1..N-1), in persistent
// memory (APs can't call boot services, and it must survive ExitBootServices).
BOOLEAN svm_alloc_ap(EFI_BOOT_SERVICES *bs, int i) {
    if (i <= 0 || i >= MAX_CPUS) return FALSE;
    ap_state_t *s = (ap_state_t *)alloc_page(bs);   // struct itself, persistent
    if (!s) return FALSE;
    s->guest_vmcb = alloc_page(bs);
    s->host_vmcb  = alloc_page(bs);
    s->hsave      = alloc_page(bs);
    UINT64 stk = 0;
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 8, &stk)))
        return FALSE;
    s->host_stack_top = stk + 8 * 4096;
    g_ap[i] = s;
    return s->guest_vmcb && s->host_vmcb && s->hsave;
}

// AP: virtualize THIS core using pre-allocated slot `i`. We intercept INIT (so
// we see the OS starting this core) plus the mandatory VMRUN. The resident loop
// is entered via the RELOCATED copy so it survives ExitBootServices.
void svm_virtualize_ap(int i) {
    ap_state_t *s = g_ap[i];
    __writemsr(MSR_VM_HSAVE_PA, s->hsave);

    vmcb *v = (vmcb *)s->guest_vmcb;
    fill_current_cpu_state(v);
    v->control.guest_asid     = (UINT32)(i + 1);
    // With AP-startup emulation ON, intercept NMI (our wake signal). With it OFF
    // (the default), intercept nothing here so the OS's native INIT/SIPI brings
    // this core up normally (virtualized by the layer beneath us) - a stable
    // multi-core Windows.
    v->control.intercept_vec3 = SMP_EMULATE_AP_STARTUP ? INTERCEPT_NMI : 0;
    // Intercept VMMCALL too, so the guest hypercall tool (minictl) works no
    // matter which core it runs on - and it proves each AP is a real guest.
    v->control.intercept_vec4 = INTERCEPT_VMRUN | INTERCEPT_VMMCALL;
    v->control.np_enable      = 1;
    v->control.n_cr3          = g_ap_npt;
    v->save.g_pat             = 0x0007040600070406ull;

    __writecr3(g_ap_hostpt);                       // host runs on shared tables
    hv_resident_fn fn = (hv_resident_fn)((UINT64)&hv_resident + g_ap_delta);
    fn(s->guest_vmcb, s->host_vmcb, &s->gctx, s->host_stack_top);
}
