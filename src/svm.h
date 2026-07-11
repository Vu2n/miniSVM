// svm.h - AMD-V (SVM) hypervisor core: detection, enable, VMCB, launch.
//
// Built up across milestones M1-M3. This header declares the public API used by
// main.c; the gritty details live in svm.c and vmrun.asm.
#pragma once
#include "efi.h"

// ---- M1: capability detection ---------------------------------------------
typedef struct {
    BOOLEAN supported;        // CPUID reports SVM exists on this CPU
    BOOLEAN usable;           // supported AND not locked off in firmware
    BOOLEAN disabled_locked;  // SVM disabled by BIOS and the lock bit is set
    UINT32  n_asids;          // number of TLB address-space IDs available
} svm_caps;

// Probe CPUID / MSRs to see whether we can use AMD-V.
svm_caps svm_detect(void);

// ---- M2: enter SVM mode ---------------------------------------------------
// Set EFER.SVME, allocate the host state-save area, program VM_HSAVE_PA.
// Returns TRUE on success. `bs` is used to allocate a page.
BOOLEAN svm_enable(EFI_BOOT_SERVICES *bs);

// ---- M3: run a guest ------------------------------------------------------
typedef struct {
    BOOLEAN launched;    // did we get far enough to execute VMRUN?
    UINT64  exits;       // number of #VMEXITs we handled before the guest exited
    UINT64  exit_code;   // final VMCB EXITCODE (-1 == invalid guest state)
    UINT64  guest_rip;   // guest RIP at the final #VMEXIT
} svm_result;

// Build a VMCB and run a tiny guest through a VMEXIT dispatch loop: the guest
// issues several VMMCALL hypercalls, and we resume it after each until it asks
// to exit.
svm_result svm_run(EFI_BOOT_SERVICES *bs);

// ---- M9: self-virtualization ----------------------------------------------
// Put the CALLER's own execution into a guest and leave the hypervisor resident
// (running as an event loop on its own stack). When this returns, the code that
// called it is running as a guest under our hypervisor - so a CPUID it now
// executes is intercepted and spoofed by us. `image` is our own ImageHandle,
// used to relocate the hypervisor into memory that survives ExitBootServices.
void svm_go_resident(EFI_HANDLE image, EFI_BOOT_SERVICES *bs);

// ---- M12: SMP (virtualize the other cores) --------------------------------
// BSP calls svm_build_ap_tables + svm_alloc_ap (per core) before starting APs.
// Each AP then calls svm_virtualize_ap(i) to self-virtualize itself.
BOOLEAN svm_build_ap_tables(EFI_BOOT_SERVICES *bs);
BOOLEAN svm_alloc_ap(EFI_BOOT_SERVICES *bs, int i);
void    svm_virtualize_ap(int i);
