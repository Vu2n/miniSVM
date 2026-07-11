# minictl — talk to miniSVM from inside Windows

A tiny console tool that runs **inside the Windows guest** and communicates with
the miniSVM hypervisor running *beneath* it, over a `VMMCALL` hypercall channel.

```
=====================================================
 miniSVM hypervisor detected BENEATH this Windows!
=====================================================
  #VMEXITs serviced   : 1832745
  Windows major ver.  : 10  (the HV read this via VMI)
  hypervisor version  : 0x10000
=====================================================
```

## Find a process by name

```
> minictl find explorer.exe
  explorer.exe -> PID 6284   (the hypervisor found this in the
                    kernel's own process list)
```

You typed a name in a user-mode program; the **hypervisor** — sitting beneath
Windows, with no driver loaded — walked the kernel's own `EPROCESS` list and
handed back the PID. That's live introspection of the OS from underneath it.

> `minictl` pins itself to CPU 0, the boot processor, which miniSVM virtualizes
> for the whole Windows session — so the `VMMCALL` always reaches the hypervisor.
> (The other cores are handed to Windows once it boots; see the SMP section in
> the architecture doc.)

## How it works

The guest puts a magic key in `RAX`, a command in `RCX`, and an optional
argument in `RDX` (for `find`, a pointer to the name string), then executes
`VMMCALL`. miniSVM intercepts `VMMCALL` (the OS never uses it), recognizes the
key, and answers in `RAX`. For `find`, it reads the name straight out of the
process's address space, walks the active-process ring, and returns the PID.
See `hvcall.asm` (the one instruction) and the `HVC_*` commands in `src/svm.c`.

## Build

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Produces `minictl.exe`.

## Run it

1. Build the hypervisor ISO with the hypercall support (`make-iso.ps1` in the
   repo root).
2. Get `minictl.exe` into the Windows VM — boot Windows **normally**, copy the
   file in (or use a VMware shared folder), then shut down.
3. Boot the VM from the miniSVM CD so Windows comes up **as miniSVM's guest**.
4. Open a Command Prompt in Windows and run `minictl.exe`.

If miniSVM is underneath, you'll see the banner above. If you run it on bare
metal (or a normal boot without the CD), it reports that no hypervisor answered.

> Note: this uses `VMMCALL` from user mode. On AMD, an intercepted `VMMCALL`
> traps to the hypervisor regardless of privilege level; the tool wraps the call
> in structured exception handling so it fails gracefully if that ever isn't the
> case.
