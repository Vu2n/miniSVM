// npt.c - Nested Page Table builder.
//
// The NPT uses the exact same 4-level format as ordinary x86-64 page tables:
//   PML4 -> PDPT -> PD -> PT, with each entry a 64-bit descriptor.
// We identity-map [0, NPT_GIB GiB) using 2 MiB leaf pages (fast to build, few
// tables), then split the single 2 MiB region that contains `redirect_gpa` down
// to 4 KiB pages so we can point that one page somewhere else.
#include "npt.h"

// Page-table entry flag bits.
#define PTE_P   (1ull << 0)   // present
#define PTE_RW  (1ull << 1)   // writable
#define PTE_US  (1ull << 2)   // user-accessible (required for NPT entries)
#define PTE_A   (1ull << 5)   // accessed
#define PTE_D   (1ull << 6)   // dirty
#define PTE_PS  (1ull << 7)   // page size: this entry is a large-page leaf

// Pre-set A/D on every entry so the CPU never needs to WRITE the page tables to
// update them during a walk (which can fault under NPT). Leaves get A+D; the
// interior table pointers get A.
#define FLAGS_TABLE (PTE_P | PTE_RW | PTE_US | PTE_A)
#define FLAGS_LEAF  (PTE_P | PTE_RW | PTE_US | PTE_A | PTE_D)

#define ADDR_MASK 0x000FFFFFFFFFF000ull   // 4 KiB-aligned physical address field

// How much guest-physical space to identity-map. 16 GiB = 16 page-directory
// pages; covers a typical VM's RAM plus its low MMIO.
#define NPT_GIB 16

// IMPORTANT: EfiRuntimeServicesData so the NPT and host page tables SURVIVE when
// a guest OS calls ExitBootServices. If these were EfiLoaderData the OS would
// reclaim and overwrite them, corrupting the page tables -> nested page faults.
static UINT64 np_alloc(EFI_BOOT_SERVICES *bs) {
    UINT64 a = 0;
    if (EFI_ERROR(bs->AllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &a)))
        return 0;
    UINT8 *p = (UINT8 *)a;
    for (UINTN i = 0; i < 4096; i++) p[i] = 0;
    return a;
}

UINT64 npt_build(EFI_BOOT_SERVICES *bs, UINT64 redirect_gpa, UINT64 redirect_hpa) {
    UINT64 pml4_pa = np_alloc(bs);
    UINT64 pdpt_pa = np_alloc(bs);
    if (!pml4_pa || !pdpt_pa) return 0;

    UINT64 *pml4 = (UINT64 *)pml4_pa;
    UINT64 *pdpt = (UINT64 *)pdpt_pa;
    pml4[0] = pdpt_pa | FLAGS_TABLE;               // first 512 GiB

    // A real redirect only exists when the two addresses differ; otherwise build
    // a pure identity map (no 4 KiB split needed).
    BOOLEAN do_redirect = (redirect_gpa != redirect_hpa);
    UINT64 redir_gib = redirect_gpa >> 30;
    UINT64 redir_2m  = (redirect_gpa >> 21) & 0x1FF;
    UINT64 redir_page = redirect_gpa & ~0xFFFull;

    for (UINT64 g = 0; g < NPT_GIB; g++) {
        UINT64 pd_pa = np_alloc(bs);
        if (!pd_pa) return 0;
        UINT64 *pd = (UINT64 *)pd_pa;

        // Identity map this 1 GiB with 512 x 2 MiB leaf pages.
        for (UINT64 j = 0; j < 512; j++) {
            UINT64 base = (g << 30) | (j << 21);
            pd[j] = base | FLAGS_LEAF | PTE_PS;
        }
        pdpt[g] = pd_pa | FLAGS_TABLE;

        // If the redirected page lives in this GiB, replace its 2 MiB leaf with
        // a page table so we can remap a single 4 KiB page inside it.
        if (do_redirect && g == redir_gib) {
            UINT64 pt_pa = np_alloc(bs);
            if (!pt_pa) return 0;
            UINT64 *pt = (UINT64 *)pt_pa;
            UINT64 region = (g << 30) | (redir_2m << 21);   // 2 MiB base GPA
            for (UINT64 k = 0; k < 512; k++) {
                UINT64 gpa = region | (k << 12);
                UINT64 hpa = (gpa == redir_page) ? redirect_hpa : gpa;
                pt[k] = (hpa & ADDR_MASK) | FLAGS_LEAF;
            }
            pd[redir_2m] = pt_pa | FLAGS_TABLE;             // now 4 KiB-granular
        }
    }
    return pml4_pa;
}

UINT64 hostpt_build(EFI_BOOT_SERVICES *bs) {
    // Same shape as npt_build's identity map, but SUPERVISOR pages (no PTE_US).
    // The hypervisor runs at CPL0 and never touches user pages, so US=0 also
    // sidesteps SMEP/SMAP faults if the firmware enabled them.
    UINT64 pml4_pa = np_alloc(bs);
    UINT64 pdpt_pa = np_alloc(bs);
    if (!pml4_pa || !pdpt_pa) return 0;

    UINT64 *pml4 = (UINT64 *)pml4_pa;
    UINT64 *pdpt = (UINT64 *)pdpt_pa;
    pml4[0] = pdpt_pa | PTE_P | PTE_RW | PTE_A;

    for (UINT64 g = 0; g < NPT_GIB; g++) {
        UINT64 pd_pa = np_alloc(bs);
        if (!pd_pa) return 0;
        UINT64 *pd = (UINT64 *)pd_pa;
        for (UINT64 j = 0; j < 512; j++) {
            UINT64 base = (g << 30) | (j << 21);       // 2 MiB supervisor leaf
            pd[j] = base | PTE_P | PTE_RW | PTE_A | PTE_D | PTE_PS;
        }
        pdpt[g] = pd_pa | PTE_P | PTE_RW | PTE_A;
    }
    return pml4_pa;
}
