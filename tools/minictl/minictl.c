// minictl.c - talk to the miniSVM hypervisor from inside the Windows guest.
//
// Run this INSIDE the Windows VM that booted under miniSVM. It issues a VMMCALL
// hypercall; if miniSVM is resident beneath Windows it answers, and we print a
// few live stats the hypervisor reports about itself and the guest.
#include <stdio.h>
#include <windows.h>

// Implemented in hvcall.asm.
extern unsigned __int64 hv_call(unsigned __int64 key, unsigned __int64 cmd);

#define HV_MAGIC    0x4D696E69534D31ull   // must match the hypervisor
#define HV_REPLY    0xC0FFEEC0FFEEull
#define HVC_PING    0
#define HVC_EXITS   1
#define HVC_WINVER  2
#define HVC_VERSION 3

int main(void) {
    unsigned __int64 reply = 0;

    // If there's no hypervisor intercepting VMMCALL, the instruction faults
    // (#UD). Catch that so we report cleanly instead of crashing.
    __try {
        reply = hv_call(HV_MAGIC, HVC_PING);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("VMMCALL faulted - nothing is intercepting it.\n");
        printf("You're either on bare metal, or not under miniSVM.\n");
        return 1;
    }

    if (reply != HV_REPLY) {
        printf("No miniSVM hypervisor detected (reply = 0x%llX).\n", reply);
        return 1;
    }

    printf("=====================================================\n");
    printf(" miniSVM hypervisor detected BENEATH this Windows! \n");
    printf("=====================================================\n");
    printf("  #VMEXITs serviced   : %llu\n", hv_call(HV_MAGIC, HVC_EXITS));
    printf("  Windows major ver.  : %llu  (the HV read this via VMI)\n",
           hv_call(HV_MAGIC, HVC_WINVER));
    printf("  hypervisor version  : 0x%llX\n", hv_call(HV_MAGIC, HVC_VERSION));
    printf("=====================================================\n");
    printf(" Every line above came from the hypervisor you are\n");
    printf(" running on top of, answered over a VMMCALL channel.\n");
    return 0;
}
