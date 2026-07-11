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

## 9. Guest ↔ hypervisor channel

`VMMCALL` is intercepted (and no OS uses it), so it's a free two-way channel. A
program inside the guest puts `HV_MAGIC` in `RAX` and a command in `RCX`, then
executes `VMMCALL`; the hypervisor recognizes the key and answers in `RAX`. The
[`tools/minictl`](../tools/minictl) program does this from inside Windows and
prints the live exit count, the hypervisor version, and the Windows version the
hypervisor read via VMI.

**Finding a process from underneath.** `minictl find explorer.exe` puts the
command in `RCX` and a pointer to the name in `RDX`. The hypervisor reads the
name out of the caller's address space, then walks the kernel's active-process
list to resolve it — the payoff of introspection:

- It starts from the current thread. `KernelGsBase` holds the `KPCR` while the
  guest is in user mode; from there `KPCR.Prcb.CurrentThread → KTHREAD.Process`
  gives a first `EPROCESS`.
- Each `EPROCESS` is on a circular doubly-linked list via `ActiveProcessLinks`.
  `vmi_find_pid` walks the ring, reading each process's `ImageFileName`, and
  returns `UniqueProcessId` on a match.
- The field offsets are for 64-bit Windows 10/11 (19041+) and are build-specific;
  every read is bounds-checked, so a wrong offset finds nothing rather than
  crashing the host.

One subtlety makes this work at all: it reads *kernel* memory using the calling
user process's `CR3`. That's only possible because AMD CPUs aren't affected by
Meltdown, so Windows leaves KPTI/KVA-shadow **off** and keeps the full kernel
mapped in every process. On an Intel host with KPTI on, you'd need the kernel
`CR3` instead.

> NPT-based write protection (clear the R/W bit on an NPT leaf, trap the `#NPF`)
> is the same mechanism HVCI uses and is easy to add here — but trapping writes
> to a live, safety-critical kernel page can stall the kernel, so it's left as
> an exercise rather than shipped armed by default.

## 10. Debugging a blind hypervisor

You can't attach a debugger to something running beneath the OS. miniSVM's answer
is **raw COM1/COM2 serial** (`dbg_*` in `svm.c`, `serial_*` in `console.c`),
written with direct port I/O so it works from the resident host context (calling
firmware from there hangs). On real hardware, point a VM serial port at a file
and watch the guest RIPs stream by. That log *is* the debugger — *during boot*.

**A trap worth knowing: serial goes dark once the OS owns the port.** Our raw
port-I/O writes to COM1 stream reliably right up until Windows loads its own
serial stack — after that, the hypervisor's writes are simply swallowed and never
reach the log file. This cost real time during SMP work: post-desktop diagnostics
looked "silent" when in fact the code was running fine and the output was lost.
The fix is to stop using serial after boot and answer through the **hypercall
channel** instead (§9) — `minictl` asks, the hypervisor stuffs the answer in `RAX`,
and it always gets through. Most of the SMP.3b diagnostics (live APIC mode,
captured ICR values, AP exit codes) are read back this way. Lesson: a resident
hypervisor needs a debugging channel the guest OS can't step on.

A second gotcha from the same work: a stale build is invisible unless you make it
visible. miniSVM prints a **`BUILD TAG`** line at boot (`main.c`); bump it every
rebuild, and if that exact string isn't in the log you booted an old ISO. (VMware
locks the `.iso` while the VM is powered on, so `xorriso` silently fails to
overwrite it — power the VM off before rebuilding.)

## 11. SMP — virtualizing the other cores (M12)

A hypervisor that only owns the boot processor loses control the moment the OS
starts the other cores. Getting *every* core under miniSVM turned into the
deepest part of this project, so it's worth telling the whole story — including
exactly where it hits a wall, because the wall is as instructive as the parts
that work.

### 11.1 Virtualizing every core at UEFI time

At UEFI time we still have the **MP Services Protocol** (`main.c`,
`find_mp_services`). `StartupAllAPs` runs `ap_virtualize` on each application
processor; each one enables SVM and calls `svm_virtualize_ap`, self-virtualizing
exactly the way the BSP did (§4). Per-core VMCB / host-save / stack / register
context are pre-allocated by the BSP (`svm_alloc_ap`, in `EfiRuntimeServicesData`,
since APs can't call boot services), and the whole image is relocated a *second*
time (`svm_relocate_aps`) so the APs' resident loops survive `ExitBootServices`.
So by the time Windows loads, **every core is already a miniSVM guest** — you can
watch each one print `this AP is now running as a guest`.

### 11.2 The handoff problem

Then Windows boots and wants those cores back. It starts each AP with the classic
**`INIT`–`SIPI`–`SIPI`** sequence over the local APIC. Owning an *already-running*
guest core across that handoff is hard, and AMD makes it harder than Intel.

**Default behaviour (`SMP_EMULATE_AP_STARTUP = 0`): let them go.** We don't
intercept `INIT` on the APs. The hardware delivers it, and on AMD an
un-intercepted `INIT` **resets the core out of SVM** — the AP leaves miniSVM's VM
and comes up one level down (bare metal, or the layer beneath us in a nested
setup). The result is a completely stable **multi-core Windows**: every core
works, the BSP stays a miniSVM guest for the whole session (VMI, hypercalls, …),
and the APs run below us. This is the shipped default.

### 11.3 SMP.3b — keeping the APs as our guests (opt-in)

Setting `SMP_EMULATE_AP_STARTUP = 1` enables the real attempt: intercept AP
startup and reset each AP *into Windows' trampoline while it stays our guest*.
The code is a tour of hypervisor techniques — here's the chain, and precisely
where it breaks.

**No SIPI exit on AMD.** Intel VMX has a wait-for-SIPI activity state and an SIPI
`#VMEXIT`; AMD SVM has neither, so we can't catch the SIPI on the *receiving* (AP)
side. Instead we watch the *sending* side — the BSP's writes to the APIC ICR —
because the BSP stays our guest.

**Windows uses legacy xAPIC (MMIO), not x2APIC.** The clean path would be
x2APIC, which sends IPIs through a single MSR (`0x830`) that's trivial to
intercept. But even with `bcdedit /set x2apicpolicy Enable`, `IA32_APIC_BASE`
bit 10 stays `0` — VMware's ACPI tables don't advertise x2APIC, so Windows can't
switch (we confirmed this over the hypercall channel). That forces the hard path:
the xAPIC ICR is memory-mapped at `0xFEE00300`/`0x310`, and the whole APIC lives
in one 4 KiB page.

**Trapping one MMIO register when NPT is page-granular.** We write-protect the
APIC page in the BSP's NPT (`npt_pte_ptr` splits the 2 MiB leaf to 4 KiB and
clears R/W). Now *every* APIC write faults — EOIs, timer, TPR, plus the ICR we
want. Each `#NPF` is handled by:

- a small **instruction decoder** (`decode_apic_write`) that reads the `mov`-to-
  memory the HAL uses (opcode `0x89`, value in a register; also `0xC7` immediates),
  and
- **single-step pass-through**: un-protect the page, set the trap flag, run
  exactly one instruction under a `#DB` intercept, then re-protect. This executes
  any instruction correctly without a full emulator. (Verified: 200/200 clean
  single-steps, Windows boots straight through it.)

Decoding the ICR writes hands us the whole sequence: delivery mode `5` = `INIT`,
`6` = `SIPI`, and the SIPI's low byte is the **trampoline vector** (`0x13` → the
AP should begin at physical `0x13000`).

**The AMD `INIT` latch.** The obvious plan — intercept `INIT` on the AP so it
stays our guest, then reset it — *loops forever*. On AMD an intercepted `INIT` is
held **pending**; the `#VMEXIT` doesn't clear it, so it re-fires on the very next
`VMRUN`, before the guest runs a single instruction. Confirmed: exit code `0x63`
on repeat, AP wedged at `RIP=0`. There is no clean VMCB field to clear the latch.

**The NMI trick.** So instead of intercepting `INIT`, we **swallow** it: when the
BSP writes an `INIT` or `SIPI` to the ICR we decode it, advance RIP past the
instruction (this is why the decoder also returns a length), and never let the
physical IPI reach the AP — nothing latches. To actually start the parked AP we
wake it with an **NMI** — edge-triggered, no latch — which we intercept on the AP
for a one-shot reset: read the published trampoline vector, build a real-mode
reset-state VMCB (`CS.base = 0x13000`, `PE = 0`), drop the NMI intercept, and
`VMRUN`. And it works: **the AP runs Windows' real-mode AP-startup trampoline as
our guest**, transitions real → protected → long mode, and enters the kernel's
AP bring-up.

### 11.4 Where it hits the wall

…and then the guest bugchecks. `vmware.log` shows, at the same instant, both
`WinBSOD: Synthetic MSR[0x40000100] 0x139` (**`KERNEL_SECURITY_CHECK_FAILURE`**,
reported through the Hyper-V crash MSR) and a VMware **`MONITOR PANIC`** on the AP
core ("invalid part of memory"). The AP got far enough to run the kernel's own
integrity-checked AP bring-up, but couldn't complete it cleanly — corrupting a
kernel structure and derailing into invalid memory.

Two things make the last inch intractable *in this environment*:

1. **Nested virtualization.** miniSVM runs under VMware, and driving a
   real-mode → long-mode AP bring-up as a *nested* guest pushes on VMware's
   nested-SVM limits — the panic is VMware's monitor, not our handler.
2. **No diagnostics on failure.** The whole VM dies on the panic, so the
   hypercall channel — our only post-boot debugging window (§10) — is gone before
   we can read anything back.

This is exactly the class of problem that would likely behave differently on
**bare metal**, where x2APIC and AVIC exist and SVM isn't itself being
virtualized. So the emulator ships **off by default** (native multi-core Windows),
fully intact behind `SMP_EMULATE_AP_STARTUP` for anyone who wants to pick it up on
real hardware.

**Bottom line:** miniSVM virtualizes every core at UEFI time and keeps the boot
processor virtualized for the entire Windows session; multi-core Windows runs,
with the APs virtualized one layer down by default. The opt-in AP-startup emulator
drives an AP all the way into Windows' own startup code — and marks the precise
point where nested virtualization runs out of road.

## What's next

- **Bare metal.** The single most likely way to finish SMP.3b: boot miniSVM on
  real hardware, where x2APIC/AVIC exist and SVM isn't nested. The x2APIC MSR path
  (already coded) would replace the whole xAPIC-MMIO dance, and the AP bring-up
  gets real fault reporting instead of a VMware monitor panic.
- **Depth over breadth.** An NPT-backed watchpoint/debugger, richer VMI (walk
  `PsLoadedModuleList`, enumerate handles), or a read/write-guest-memory API over
  the hypercall channel. The bones are all here to build on.
