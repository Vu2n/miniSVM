// npt.h - Nested Page Table (NPT) builder for AMD-V.
//
// NPT adds a second level of address translation: the guest's own page tables
// map guest-virtual -> guest-physical (GPA), and the NPT maps guest-physical ->
// host-physical (HPA). The hypervisor owns the NPT, so it controls exactly
// which host memory backs each guest-physical page - the essence of memory
// virtualization and isolation.
#pragma once
#include "efi.h"

// Build an NPT that identity-maps guest-physical == host-physical across a large
// low range (so the guest runs normally), EXCEPT one 4 KiB page: accesses to
// guest-physical `redirect_gpa` are steered to host-physical `redirect_hpa`.
//
// Returns the host-physical address of the NPT's top-level table (to load into
// VMCB.N_CR3), or 0 on allocation failure. Both addresses must be page-aligned.
UINT64 npt_build(EFI_BOOT_SERVICES *bs, UINT64 redirect_gpa, UINT64 redirect_hpa);

// Build ordinary (not nested) 4-level page tables that identity-map physical
// memory as SUPERVISOR pages (US=0), for use as the hypervisor's own host CR3.
// This decouples the resident host from the firmware/OS page tables (which get
// torn down once an OS takes over). Returns the PML4 host-physical address, or 0.
UINT64 hostpt_build(EFI_BOOT_SERVICES *bs);

// Ensure the 2 MiB region containing `gpa` in the NPT rooted at `pml4_pa` is
// broken down to 4 KiB pages (splitting the large leaf on demand, using `bs` to
// allocate the page table), and return a pointer to the 4 KiB PTE for `gpa`. The
// caller can then clear/set the writable bit (1<<1) to trap or allow writes to
// that single page (e.g. the local APIC MMIO page). Returns NULL on failure.
// Must be called at build time (before ExitBootServices) since it allocates.
UINT64 *npt_pte_ptr(EFI_BOOT_SERVICES *bs, UINT64 pml4_pa, UINT64 gpa);
