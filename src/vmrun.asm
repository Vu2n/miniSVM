; vmrun.asm - SVM assembly glue.
;
; Assembled with NASM as win64 COFF so it links with MSVC (link.exe). Functions
; follow the Microsoft x64 ABI: integer args in RCX, RDX, R8, R9; callee must
; preserve RBX, RBP, RDI, RSI, R12-R15, RSP.
default rel
section .text

global capture_host_desc
global svm_launch
global guest_entry
global hv_resident
global __chkstk

extern hv_handle_exit      ; C: void hv_handle_exit(vmcb *v, guest_gprs *g)

; VMCB state-save-area field offsets we poke directly (AMD APM Appendix B).
%define G_RFLAGS 0x570
%define G_RIP    0x578
%define G_RSP    0x5D8
%define G_RAX    0x5F8

; -----------------------------------------------------------------------------
; void capture_host_desc(host_desc *p)   ; RCX = p (a #pragma pack(1) struct)
;
; Snapshot the segment selectors and the GDTR/IDTR of the currently-running
; host so we can hand equivalent (valid) state to the guest.
; -----------------------------------------------------------------------------
capture_host_desc:
    mov     [rcx+0],  cs
    mov     [rcx+2],  ss
    mov     [rcx+4],  ds
    mov     [rcx+6],  es
    mov     [rcx+8],  fs
    mov     [rcx+10], gs
    str     word [rcx+12]     ; Task Register selector
    sldt    word [rcx+14]     ; LDT selector
    sgdt    [rcx+16]          ; 2-byte limit + 8-byte base (packed)
    sidt    [rcx+26]          ; 2-byte limit + 8-byte base (packed)
    ret

; -----------------------------------------------------------------------------
; void svm_launch(UINT64 guest_vmcb_pa, UINT64 host_vmcb_pa, guest_gprs *gprs)
;                 ; RCX = guest_vmcb_pa, RDX = host_vmcb_pa, R8 = gprs
;
; Perform one world switch into the guest and back. VMRUN preserves the host's
; core state (CS/SS/DS/ES, CRs, GDTR/IDTR, RIP/RSP/RAX/RFLAGS) via VM_HSAVE_PA
; automatically, and RAX/RSP/RIP for the guest live in the VMCB. Everything else
; we manage by hand:
;   - the host's FS/GS/TR/LDTR + syscall MSRs (VMSAVE/VMLOAD around VMRUN)
;   - the guest's general-purpose registers, which are NOT in the VMCB: we load
;     them from *gprs before VMRUN and save them back after, so the guest keeps
;     its register state across successive exits.
; -----------------------------------------------------------------------------
svm_launch:
    push    rbx
    push    rbp
    push    rsi
    push    rdi
    push    r12
    push    r13
    push    r14
    push    r15
    push    r8                  ; [rsp+16] = gprs
    push    rdx                 ; [rsp+8]  = host_vmcb_pa
    push    rcx                 ; [rsp+0]  = guest_vmcb_pa

    mov     rax, rcx            ; -> guest VMCB
    vmsave                      ; copy host FS/GS/TR/LDTR/MSRs into guest VMCB
    mov     rax, [rsp+8]        ; -> host VMCB
    vmsave                      ; stash the same host extra-state for restore

    clgi                        ; mask interrupts during the world switch

    ; Load the guest's general-purpose registers from the context block.
    mov     rax, [rsp+16]       ; -> gprs
    mov     rbx, [rax+0]
    mov     rcx, [rax+8]
    mov     rdx, [rax+16]
    mov     rsi, [rax+24]
    mov     rdi, [rax+32]
    mov     rbp, [rax+40]
    mov     r8,  [rax+48]
    mov     r9,  [rax+56]
    mov     r10, [rax+64]
    mov     r11, [rax+72]
    mov     r12, [rax+80]
    mov     r13, [rax+88]
    mov     r14, [rax+96]
    mov     r15, [rax+104]

    mov     rax, [rsp+0]        ; -> guest VMCB (RAX holds the VMCB PA for VMRUN)
    vmload                      ; load guest extra-state
    vmrun                       ; ===== ENTER GUEST; returns on #VMEXIT =====
    ; On return: guest GPRs are live in RBX..R15; RAX = guest VMCB PA (the host
    ; RAX value, restored from VM_HSAVE_PA). The guest's own RAX/RSP/RIP are in
    ; the VMCB. RSP is unchanged, so our stack slots are still valid.

    ; Save the guest's general-purpose registers back into the context block.
    mov     rax, [rsp+16]       ; -> gprs
    mov     [rax+0],   rbx
    mov     [rax+8],   rcx
    mov     [rax+16],  rdx
    mov     [rax+24],  rsi
    mov     [rax+32],  rdi
    mov     [rax+40],  rbp
    mov     [rax+48],  r8
    mov     [rax+56],  r9
    mov     [rax+64],  r10
    mov     [rax+72],  r11
    mov     [rax+80],  r12
    mov     [rax+88],  r13
    mov     [rax+96],  r14
    mov     [rax+104], r15

    mov     rax, [rsp+8]        ; -> host VMCB
    vmload                      ; restore host FS/GS/TR/LDTR/MSRs
    stgi                        ; re-enable interrupts

    add     rsp, 24             ; drop the three saved args
    pop     r15
    pop     r14
    pop     r13
    pop     r12
    pop     rdi
    pop     rsi
    pop     rbp
    pop     rbx
    ret

; -----------------------------------------------------------------------------
; The guest. Executes in guest mode with CPL=0, sharing the host's page tables.
; It demonstrates two hypervisor capabilities, then exits:
;
;   M5 (spoof CPUID): ask the CPU for its vendor via CPUID leaf 0 - which the
;       hypervisor intercepts and answers with a fake vendor - then hand that
;       vendor back so the hypervisor can print what the guest "saw".
;
;   M6 (guest memory R/W): write "PING" into a buffer and give the hypervisor
;       the pointer; the hypervisor reads it and overwrites it with "PONG HV!";
;       the guest reads the buffer back and reports what it now contains.
;
; Every CPUID/VMMCALL is intercepted and causes a #VMEXIT; the hypervisor
; services it, advances RIP, and resumes us.
; -----------------------------------------------------------------------------
guest_entry:
    ; --- M5: CPUID vendor spoof ---
    xor     eax, eax            ; CPUID leaf 0 (vendor string)
    cpuid                       ; EBX,EDX,ECX = vendor (spoofed by the hypervisor)
    mov     eax, 0x10           ; HC_REPORT_VENDOR; RBX/RDX/RCX still hold vendor
    vmmcall

    ; --- M6: guest memory read/write ---
    lea     rbx, [rel guest_buf] ; RBX = &buffer (shared address space here)
    mov     rax, 0x474E4950      ; bytes 'P','I','N','G',0,0,0,0
    mov     [rbx], rax           ; guest writes "PING" into its buffer
    mov     eax, 0x20            ; HC_PROCESS (RBX = buffer pointer)
    vmmcall                      ; -> hypervisor reads + overwrites the buffer
    mov     rdx, [rbx]           ; guest reads back the (HV-modified) buffer
    mov     eax, 0x21            ; HC_REPORT (RDX = the 8 bytes it read)
    vmmcall

    ; --- M7: nested paging ---
    ; RDI holds a guest-physical address the hypervisor redirected via NPT.
    ; We write to it; the data actually lands in a different host page.
    mov     rax, 'INSIDEVM'      ; 8 ASCII bytes to store
    mov     [rdi], rax           ; guest writes to GPA in RDI -> NPT -> host page R
    mov     eax, 0x30            ; HC_NPT_DONE
    vmmcall

    ; --- done ---
    xor     eax, eax             ; HC_EXIT (0)
    vmmcall
.hang:                           ; safety net; should be unreachable
    hlt
    jmp     .hang

section .data
guest_buf:  times 32 db 0        ; scratch buffer the guest and hypervisor share

section .text
; -----------------------------------------------------------------------------
; UINT64 hv_resident(UINT64 guest_vmcb, UINT64 host_vmcb,
;                    guest_gprs *g, UINT64 host_stack_top)
;   RCX = guest_vmcb, RDX = host_vmcb, R8 = g, R9 = host_stack_top
;
; SELF-VIRTUALIZATION. The VMCB has already been filled with the current CPU's
; segment/CR/EFER/GDTR/IDTR state, intercepts, and NPT. Here we point the guest
; at OUR caller's continuation (guest RIP = our return address, guest RSP = our
; caller's stack) and enter the guest. The effect: hv_resident "returns" (once)
; already running as a guest, with RAX = 1.
;
; From then on the hypervisor lives as an event loop on host_stack_top: each
; #VMEXIT saves the guest, calls the C handler, and re-enters. It never returns
; normally - the resident hypervisor keeps servicing the guest (which is now the
; rest of our own execution).
; -----------------------------------------------------------------------------
hv_resident:
    ; Snapshot the caller's GPRs into *g so the guest continuation keeps them.
    mov     [r8+0],   rbx
    mov     [r8+8],   rcx
    mov     [r8+16],  rdx
    mov     [r8+24],  rsi
    mov     [r8+32],  rdi
    mov     [r8+40],  rbp
    mov     [r8+48],  r8
    mov     [r8+56],  r9
    mov     [r8+64],  r10
    mov     [r8+72],  r11
    mov     [r8+80],  r12
    mov     [r8+88],  r13
    mov     [r8+96],  r14
    mov     [r8+104], r15

    ; Guest resumes at our return address, on the caller's stack, with RAX = 1.
    mov     rax, [rsp]                 ; return address
    mov     [rcx + G_RIP], rax
    lea     rax, [rsp+8]               ; caller RSP after 'ret'
    mov     [rcx + G_RSP], rax
    pushfq
    pop     rax
    mov     [rcx + G_RFLAGS], rax
    mov     qword [rcx + G_RAX], 1

    ; Switch to the dedicated host stack and stash the three pointers in a frame
    ; that survives across VMRUN (host RSP is preserved by VM_HSAVE).
    mov     rsp, r9
    sub     rsp, 32
    mov     [rsp+0],  rcx              ; guest_vmcb
    mov     [rsp+8],  rdx              ; host_vmcb
    mov     [rsp+16], r8              ; g (guest_gprs*)

    ; Seed the guest's VMSAVE-state (FS/GS/TR/LDTR/MSRs) from the current host.
    mov     rax, rcx
    vmsave

.loop:
    clgi                              ; mask interrupts across the world switch
    mov     rax, [rsp+8]              ; host_vmcb
    vmsave                            ; save host FS/GS/TR/LDTR/MSRs
    mov     rax, [rsp+0]              ; guest_vmcb
    vmload                            ; load guest FS/GS/TR/LDTR/MSRs

    mov     rax, [rsp+16]            ; g -> load guest GPRs
    mov     rbx, [rax+0]
    mov     rcx, [rax+8]
    mov     rdx, [rax+16]
    mov     rsi, [rax+24]
    mov     rdi, [rax+32]
    mov     rbp, [rax+40]
    mov     r8,  [rax+48]
    mov     r9,  [rax+56]
    mov     r10, [rax+64]
    mov     r11, [rax+72]
    mov     r12, [rax+80]
    mov     r13, [rax+88]
    mov     r14, [rax+96]
    mov     r15, [rax+104]

    mov     rax, [rsp+0]             ; guest_vmcb PA for VMRUN
    vmrun                            ; ===== run guest until #VMEXIT =====

    mov     rax, [rsp+16]           ; g -> save guest GPRs back
    mov     [rax+0],   rbx
    mov     [rax+8],   rcx
    mov     [rax+16],  rdx
    mov     [rax+24],  rsi
    mov     [rax+32],  rdi
    mov     [rax+40],  rbp
    mov     [rax+48],  r8
    mov     [rax+56],  r9
    mov     [rax+64],  r10
    mov     [rax+72],  r11
    mov     [rax+80],  r12
    mov     [rax+88],  r13
    mov     [rax+96],  r14
    mov     [rax+104], r15

    mov     rax, [rsp+0]             ; guest_vmcb
    vmsave                           ; capture guest FS/GS/TR/LDTR/MSR changes
    mov     rax, [rsp+8]             ; host_vmcb
    vmload                           ; restore host FS/GS/TR/LDTR/MSRs
    ; GIF is 0 here (cleared by #VMEXIT), so the C handler runs uninterrupted.

    mov     rcx, [rsp+0]            ; arg1: guest_vmcb
    mov     rdx, [rsp+16]           ; arg2: g
    sub     rsp, 32                  ; Win64 shadow space (keeps 16-byte align)
    call    hv_handle_exit
    add     rsp, 32
    jmp     .loop

; -----------------------------------------------------------------------------
; __chkstk stub. MSVC inserts calls to __chkstk to probe stack pages for large
; frames. UEFI has already committed our stack, so probing is unnecessary; the
; contract (RAX = frame size in, RSP unchanged, RAX preserved) is satisfied by a
; bare return.
; -----------------------------------------------------------------------------
__chkstk:
    ret
