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

## How it works

The guest puts a magic key in `RAX` and a command in `RCX`, then executes
`VMMCALL`. miniSVM intercepts `VMMCALL` (the OS never uses it), recognizes the
key, and answers in `RAX`. See `hvcall.asm` (the one instruction) and the
`HVC_*` commands in `src/svm.c`.

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
