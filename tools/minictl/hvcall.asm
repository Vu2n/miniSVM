; hvcall.asm - execute VMMCALL to talk to the miniSVM hypervisor from Windows.
;
; MSVC x64 has no inline assembly, so this one function is assembled with ml64.
; Microsoft x64 ABI: arg1 (key) arrives in RCX, arg2 (cmd) in RDX; return in RAX.
.code
hv_call proc
    mov     rax, rcx            ; RAX = HV_MAGIC handshake key
    mov     rcx, rdx            ; RCX = command number
    db      0Fh, 01h, 0D9h      ; vmmcall  (opcode 0F 01 D9)
    ret                         ; RAX = the hypervisor's reply
hv_call endp
end
