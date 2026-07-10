# miniSVM — a from-scratch AMD-V hypervisor that boots Windows

miniSVM is a tiny, heavily-commented **Type-1 hypervisor** written from nothing.
It boots as a UEFI application, enters **AMD-V (SVM)**, virtualizes its *own*
running code, survives the OS handoff, and then **boots a real, unmodified
Windows install as its guest — all the way to the desktop — on physical
hardware.** Along the way it reads live kernel memory out of the running OS and
traps writes to protected pages.

No EDK2, no gnu-efi, no hypervisor framework. Just **MSVC + NASM + Python +
xorriso**, and about a dozen small, readable source files. It's built as a
learning resource: each capability is a milestone you can read top-to-bottom.

> ⚠️ **Educational project.** miniSVM is transparent by design — it announces
> itself and never hides from the guest. It's meant for learning how hardware
> virtualization actually works, on machines you own. Test it in a VM.

---

## It really boots Windows — here's the proof

miniSVM logs to COM1. This is (trimmed) serial output from a real boot on an
AMD machine, captured over a virtual serial port:

```
[M11] resident; chainloading Windows Boot Manager...
[chain] launching Windows Boot Manager as our guest...
[hv] guest rip=0x00000000101527B9      <- Windows Boot Manager, as our guest
[hv] guest rip=0x0000000000B44799       <- winload (the OS loader)
...
[hv] *** Windows KERNEL running as our guest! ***

[VMI] === Hypervisor reading LIVE Windows kernel memory ===
[VMI]   NtSystemRoot   : C:\Windows
[VMI]   NtMajorVersion : 0x000000000000000A     (Windows 10 / 11)
[VMI] === we just read that out of Windows' own address space ===

[MEMPROT] locked KUSER_SHARED_DATA's page READ-ONLY;
[hv] disabling CPUID intercept -> guest now runs at native speed.
[MEMPROT] caught guest write to locked page; RIP=0xFFFFF8017F9FF574
```

Every `guest rip=` line is an instruction of the real OS that trapped out to our
hypervisor and was resumed by it. The `0xFFFFF8…` addresses are `ntoskrnl`
itself, executing under us.

---

## Features

- 🧩 **Boots as a UEFI app** — the CPU arrives already in 64-bit long mode, so
  there's *no* bootloader and no real-mode assembly. The code is about the
  hypervisor, not mode switches.
- 🔍 **Full AMD-V (SVM) engine** — VMCB setup, `VMRUN` world-switch loop,
  `#VMEXIT` dispatch, with a persistent guest register context.
- 🧠 **Nested paging (NPT)** — the guest gets its own physical address space;
  includes a demo that redirects a guest-physical page to prove control.
- 🎭 **CPUID control** — spoof the guest's view of the CPU; advertise the
  hypervisor at the standard `0x40000000` leaf.
- ✍️ **Guest memory read/write** from the hypervisor.
- ♻️ **Self-virtualization** — miniSVM puts its *own* running execution into a
  guest and keeps running as a resident event loop underneath it.
- 🪄 **Survives `ExitBootServices`** — relocates itself (with PE base-relocation
  fixups) into memory the OS won't reclaim, and builds its own page tables, so
  it lives on after the firmware is torn down.
- 🪟 **Chainloads real Windows** — finds and launches `bootmgfw.efi` *into* the
  guest, so Windows boots underneath the hypervisor deterministically.
- 🕵️ **VMI (Virtual Machine Introspection)** — walks the guest's page tables and
  reads live Windows kernel memory (`NtSystemRoot`, version, …). This is the
  exact technique EDR/forensics tools use.
- 🛡️ **NPT memory-write protection** — locks a page read-only and traps the
  guest writing to it, naming the offending kernel RIP. The core idea behind
  HVCI / kernel-integrity enforcement.
- 🔬 **Runs on real AMD hardware** (validated on VMware Workstation with
  *Virtualize AMD-V/RVI*), and **in QEMU** for fast iteration.

---

## The milestones (the learning path)

The project is built up in readable stages. Reading them in order *is* the
tutorial.

| # | Milestone | What you learn |
|---|-----------|----------------|
| M0 | Boot as a UEFI app, print | UEFI entry, freestanding C |
| M1 | Detect AMD-V via `CPUID` | feature detection, MSRs |
| M2 | Enter SVM mode | `EFER.SVME`, host save area |
| M3–M4 | `VMRUN` a guest, dispatch `#VMEXIT`s | the VMCB, the world-switch loop |
| M5 | Intercept & spoof `CPUID` | controlling the guest's CPU view |
| M6 | Read/write guest memory | hypercalls, memory access |
| M7 | Nested paging (NPT) | second-level address translation |
| M8 | Bootable ISO | packaging (FAT + El Torito) |
| M9 | **Self-virtualization** | virtualizing your own execution |
| M10 | Persist + own page tables + survive `ExitBootServices` | staying resident |
| M11 | **Chainload Windows as a guest** | hosting a real OS |
| + | VMI, memory protection | introspection & integrity |

*(Next up: M12 — SMP, so the guest can use more than one core.)*

---

## How it works (the interesting bits)

**Self-virtualization.** Rather than run a toy guest, miniSVM builds a VMCB whose
guest state *is the current CPU state*, points guest RIP at its own caller's
return address, and executes `VMRUN`. The call "returns" already running as a
guest, while the hypervisor continues as an event loop on a separate stack. From
that point on, everything that executes — the firmware, the boot manager, and
eventually Windows — runs underneath it.

**Surviving the OS.** When Windows calls `ExitBootServices`, it reclaims all
boot-services memory. miniSVM copies itself into `EfiRuntimeServicesData` (with
PE base-relocation fixups so the copy is self-contained), builds its own
supervisor page tables, and runs the resident handler from the copy — so it
keeps working after the firmware is gone.

**Hosting Windows.** After going resident, miniSVM locates
`\EFI\Microsoft\Boot\bootmgfw.efi`, builds a device path, and `LoadImage` +
`StartImage`s it *inside* the guest. Windows boots normally, unaware it's a
guest, at near-native speed (miniSVM drops `CPUID` interception once the kernel
is up).

**Seeing inside.** VMI translates a guest *virtual* address by walking the
guest's own page tables (root = guest `CR3`) and then the NPT, landing on a host
pointer it can read — no cooperation from the guest required.

---

## Build & run

**Requirements:** Windows + Visual Studio 2022 (MSVC), NASM, QEMU (ships OVMF),
xorriso, Python 3. All detected/used by the scripts.

```powershell
# Build + boot in QEMU (fast local test of M0–M9)
powershell -ExecutionPolicy Bypass -File test.ps1

# Build a UEFI-bootable ISO and validate it in QEMU
powershell -ExecutionPolicy Bypass -File make-iso.ps1
```

### Booting Windows under it (real AMD hardware)

1. A VMware Workstation VM with **UEFI firmware**, **Virtualize AMD-V/RVI** on,
   and **1 vCPU** (SMP isn't implemented yet).
2. Inside Windows, disable its own hypervisor so it doesn't fight for AMD-V:
   turn off *Memory Integrity*, *Hyper-V*, *Virtual Machine Platform*, and
   `bcdedit /set hypervisorlaunchtype off`.
3. Attach `miniSVM.iso` to the VM's CD, add a serial port → file (for the log),
   and boot from the CD. miniSVM goes resident and chainloads Windows.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for a deeper tour.

---

## Repository layout

```
src/efi.h        minimal, self-contained UEFI definitions (no SDK)
src/cpu.h        CPU intrinsics (CPUID, MSR, CR, port I/O)
src/console.c/.h UEFI console + raw COM1/COM2 serial logging
src/svm.c/.h     the hypervisor: detect, enable, VMCB, exit handler, VMI, memprot
src/npt.c/.h     nested + host page-table builders
src/vmrun.asm    VMRUN world-switch, self-virt resident loop, host-state capture
src/main.c       entry point, milestone orchestration, chainload
tools/make_esp.py  hand-built FAT16 EFI System Partition image
build.ps1 / test.ps1 / make-iso.ps1   build & test
```

---

## Status & caveats

- Single vCPU only (SMP / multi-core is the next milestone).
- Identity-maps 16 GiB of guest-physical space (fine for typical VM RAM).
- Transparent, non-stealth, non-persistent-across-reboot — by design.

## License

MIT — see [LICENSE](LICENSE). Built as a base for others to learn from; fork it,
break it, extend it.
