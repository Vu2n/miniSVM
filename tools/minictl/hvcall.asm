; hvcall.asm - execute VMMCALL to talk to the miniSVM hypervisor from Windows.
;
; MSVC x64 has no inline assembly, so this one function is assembled with ml64.
; Microsoft x64 ABI: arg1 (key) in RCX, arg2 (cmd) in RDX, arg3 (arg) in R8.
; The hypervisor reads key in RAX, cmd in RCX, arg in RDX; return in RAX.
.code
hv_call proc
    mov     rax, rcx            ; RAX = HV_MAGIC handshake key
    mov     rcx, rdx            ; RCX = command number
    mov     rdx, r8             ; RDX = optional argument (e.g. a pointer)
    db      0Fh, 01h, 0D9h      ; vmmcall  (opcode 0F 01 D9)
    ret                         ; RAX = the hypervisor's reply
hv_call endp
end
