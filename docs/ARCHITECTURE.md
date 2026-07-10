# miniSVM architecture

A guided tour of how a from-scratch UEFI hypervisor ends up running Windows.
Read this alongside the source — every claim here maps to code you can open.

## 1. The shape of the thing

miniSVM is a **Type-1 (bare-metal) hypervisor** that installs itself during UEFI
boot and then hosts whatever boots next. Its life has three phases:

1. **Demo phase** (`main.c`, M0–M8): a normal UEFI app that detects AMD-V, runs
   a small toy guest, and shows off CPUID spoofing, guest memory R/W, and NPT.
2. **Going resident** (M9–M10): it virtualizes its *own* execution and relocates
   itself into memory the OS can't reclaim.
3. **Hosting** (M11+): it chainloads Windows, which runs as its guest; the
   hypervisor lives on as an event loop servicing `#VMEXIT`s.

## 2. AMD-V essentials used here

- **VMCB** (`vmcb` in `svm.c`) — a 4 KiB control block: a *control area*
  (intercepts, ASID, NPT root, exit info) and a *state-save area* (the guest's
  segments, CRs, RIP/RSP/RFLAGS, …). Its field offsets are pinned by the AMD
  spec; a compile-time `sizeof` check guards the layout.
- **`VMRUN`** — enters the guest; returns on `#VMEXIT`. It auto-saves/restores a
  subset of host state via `VM_HSAVE_PA`; everything else (GPRs, FS/GS/TR/LDTR)
  the hypervisor manages by hand in `vmrun.asm`.
- **Intercepts** — miniSVM intercepts `CPUID` and `VMMCALL` (and the mandatory
  `VMRUN`) during setup, then drops `CPUID` interception once Windows is up, for
  speed.

## 3. The world-switch loop

`vmrun.asm` holds the heart of it. `hv_resident` (used for the real OS) and
`svm_launch` (used for the toy guest) both:

1. save host GPRs, `VMSAVE` host extra-state,
2. `CLGI`, load guest GPRs from a persistent context,
3. `VMRUN`,
4. on `#VMEXIT`, save guest GPRs, `VMLOAD` host extra-state,
5. call the C handler `hv_handle_exit`, then loop.

The guest's GPRs live in a `guest_gprs` block (not in the VMCB), so they persist
across exits. This is the piece most tutorials get subtly wrong.

## 4. Self-virtualization (M9)

The trick that turns a "run a guest" demo into a Type-1 hypervisor: build a VMCB
whose guest state is the *current* CPU state, set guest RIP to `hv_resident`'s
return address and guest RSP to the caller's stack, then `VMRUN`. The function
"returns" already executing as a guest, while the host loop runs forever on a
dedicated stack. Everything downstream — firmware, boot manager, Windows — is now
a guest.

Key gotcha: the exit handler must advance RIP using `nrip` *with a fallback*
(`nrip ? nrip : rip + len`); if `nrip` is 0 the guest jumps to address 0 and
dies.

## 5. Surviving `ExitBootServices` (M10)

When an OS takes over it reclaims `EfiLoaderData`/`EfiBootServicesData`. So:

- **M10.1** — the host gets its **own** supervisor page tables
  (`hostpt_build`), so it no longer depends on the firmware's (which vanish).
- **M10.2** — miniSVM copies its whole image into `EfiRuntimeServicesData` and
  applies **PE base relocations** to the copy (`relocate_hv_image`), then runs
  the resident handler from the copy. All runtime allocations (VMCB, NPT, host
  stack) are `EfiRuntimeServicesData` too.
- **M10.3** — verified by having the app call `ExitBootServices` itself and
  checking the hypervisor still services its exits afterward.

## 6. Nested paging (`npt.c`)

The NPT is an ordinary 4-level page table used for guest-physical → host-physical
translation. miniSVM identity-maps 16 GiB with 2 MiB leaves (A/D bits pre-set so
the CPU never needs to write them mid-walk — a subtle source of nested page
faults if you forget). `n_cr3` in the VMCB points at it; `NP_ENABLE` turns it on.

## 7. Chainloading Windows (M11)

`chainload_windows` (`main.c`) enumerates Simple File System handles, finds the
one holding `\EFI\Microsoft\Boot\bootmgfw.efi`, appends a media file-path node to
that device's device path, and `LoadImage` + `StartImage`s it. Because miniSVM is
already resident, the Windows Boot Manager (and everything it loads) runs as a
guest.

## 8. Introspection (VMI)

`guest_va_to_host` walks the guest's own page tables (root = guest `CR3` in the
VMCB) to turn a guest virtual address into a guest-physical one, then relies on
the identity NPT + identity host page tables to dereference it. `hv_vmi_windows`
uses this to read `KUSER_SHARED_DATA` (a page every Windows maps at a fixed
kernel address) and print `NtSystemRoot` and the OS version — read straight out
of the live guest.

## 9. Memory-write protection

The hypervisor clears the R/W bit on the NPT leaf covering a chosen page and
flushes the nested TLB (`VMCB.TLB_CONTROL`). The next guest write faults with
`#NPF`; the handler identifies the offending guest RIP, restores R/W, and
resumes. It's the same mechanism as HVCI/kernel-integrity — applied here to
`KUSER_SHARED_DATA` as a one-shot demo (that page is safety-critical, so the
catch is deliberately fast to avoid stalling the kernel).

## 10. Debugging a blind hypervisor

You can't attach a debugger to something running beneath the OS. miniSVM's answer
is **raw COM1/COM2 serial** (`dbg_*` in `svm.c`, `serial_*` in `console.c`),
written with direct port I/O so it works from the resident host context (calling
firmware from there hangs). On real hardware, point a VM serial port at a file
and watch the guest RIPs stream by. That log *is* the debugger.

## What's next (M12)

SMP. Windows currently must run on 1 vCPU because miniSVM virtualizes only the
boot processor. Multi-core means intercepting AP startup (`INIT`/`SIPI`) and
giving each core its own VMCB and world-switch. That's the frontier.
