// minictl.c - talk to the miniSVM hypervisor from inside the Windows guest.
//
// Run this INSIDE the Windows VM that booted under miniSVM. It issues a VMMCALL
// hypercall; if miniSVM is resident beneath Windows it answers, and we print a
// few live stats the hypervisor reports about itself and the guest.
//
// Usage:
//   minictl                 - show hypervisor + guest stats
//   minictl find <name>     - ask the hypervisor for a process's PID by name,
//                             e.g.  minictl find explorer.exe
//   minictl core <N> ...    - run the hypercall on CPU N (diagnostic; default 0)
//
// We pin ourselves to CPU 0 (the boot processor) by default: that core is the
// one miniSVM virtualizes for the whole life of the system, so its VMMCALL
// channel is always available. (The application processors are virtualized at
// UEFI time, but whether they stay our guests once Windows brings them online is
// a separate question - use `minictl core <N>` to probe a specific core.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <intrin.h>

// Implemented in hvcall.asm. Third arg is an optional value (e.g. a pointer).
extern unsigned __int64 hv_call(unsigned __int64 key, unsigned __int64 cmd,
                                unsigned __int64 arg);

#define HV_MAGIC     0x4D696E69534D31ull   // must match the hypervisor
#define HV_REPLY     0xC0FFEEC0FFEEull
#define HVC_PING     0
#define HVC_EXITS    1
#define HVC_WINVER   2
#define HVC_VERSION  3
#define HVC_FIND_PID 4
#define HVC_APICBASE  5
#define HVC_SMP_COUNT 6
#define HVC_SMP_SIPI  7
#define HVC_APIC_FAULTS 8
#define HVC_APIC_RIP    9
#define HVC_APIC_B0     10
#define HVC_APIC_B1     11
#define HVC_APIC_GPA    12
#define HVC_APIC_INFO1  13
#define HVC_DB_COUNT    14
#define HVC_ICR_LO      15
#define HVC_ICR_HI      16
#define HVC_ICR_COUNT   17
#define HVC_INIT_CNT    18
#define HVC_SIPI_CNT    19
#define HVC_SIPI_VEC    20
#define HVC_APIC_UNDEC  21
#define HVC_AP_STARTED  22
#define HVC_AP_BADEXIT  23
#define HVC_AP_BADRIP   24
#define HVC_AP_BADCOUNT 25
#define HVC_RM_EXIT     26
#define HVC_RM_RIP      27
#define HVC_INIT_LOOPS  28

// Confirm miniSVM answers on the current core. Returns 1 if so; prints an
// explanation and returns 0 otherwise.
static int hv_present(void) {
    unsigned __int64 reply = 0;
    // If nothing intercepts VMMCALL, the instruction faults (#UD). Catch it.
    __try {
        reply = hv_call(HV_MAGIC, HVC_PING, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        printf("VMMCALL faulted - nothing is intercepting it on this core.\n");
        printf("You're either on bare metal, or this core isn't our guest.\n");
        return 0;
    }
    if (reply != HV_REPLY) {
        printf("No miniSVM answer on this core (reply = 0x%llX).\n", reply);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    int argi = 1;

    // Optional: "core <N>" selects which CPU to run the hypercall on.
    DWORD_PTR mask = 1;                       // default: CPU 0 (the BSP)
    if (argi + 1 < argc && _stricmp(argv[argi], "core") == 0) {
        mask = (DWORD_PTR)1 << atoi(argv[argi + 1]);
        argi += 2;
    }
    // Pin to the chosen core, then SPIN (yielding) until we're actually running
    // on it before issuing the VMMCALL. Setting affinity alone races: the thread
    // keeps running on its current core until the next scheduling point, so a
    // hypercall issued immediately can still land on the wrong core.
    SetProcessAffinityMask(GetCurrentProcess(), mask);
    SetThreadAffinityMask(GetCurrentThread(), mask);
    for (int i = 0; i < 200 && ((mask >> GetCurrentProcessorNumber()) & 1) == 0; i++)
        Sleep(1);
    printf("(running on CPU %u)\n", GetCurrentProcessorNumber());

    // "apic": ask the CPU (via CPUID, no hypervisor needed) whether this virtual
    // CPU even supports x2APIC. This decides whether the clean AP-virtualization
    // path (x2APIC MSR intercept) is possible in this VM at all.
    if (argi < argc && _stricmp(argv[argi], "apic") == 0) {
        int r[4];
        __cpuid(r, 1);
        printf("  CPUID.1:EDX[9]  APIC (xAPIC) supported : %d\n", (r[3] >> 9) & 1);
        printf("  CPUID.1:ECX[21] x2APIC supported       : %d\n", (r[2] >> 21) & 1);
        if (((r[2] >> 21) & 1) == 0)
            printf("  -> this virtual CPU has NO x2APIC; Windows must use xAPIC\n"
                   "     (MMIO). The clean MSR intercept path isn't available here.\n");
        else
            printf("  -> x2APIC IS available; `bcdedit /set x2apicpolicy Enable`\n"
                   "     + a full shutdown should switch Windows onto it.\n");
        return 0;
    }

    if (!hv_present())
        return 1;

    // "smp": read the SMP.3b diagnostics back through the hypercall channel
    // (reliable, unlike the hypervisor's post-boot serial). Tells us the live
    // APIC mode and whether the ICR intercept caught Windows starting the APs.
    if (argi < argc && _stricmp(argv[argi], "smp") == 0) {
        unsigned __int64 ab = hv_call(HV_MAGIC, HVC_APICBASE, 0);
        unsigned __int64 n  = hv_call(HV_MAGIC, HVC_SMP_COUNT, 0);
        unsigned __int64 s  = hv_call(HV_MAGIC, HVC_SMP_SIPI, 0);
        printf("  IA32_APIC_BASE       : 0x%llX   (x2APIC %s)\n",
               ab, (ab & 0x400) ? "ENABLED" : "OFF - Windows on xAPIC");
        printf("  INIT/SIPI seen by HV : %llu\n", n);
        printf("  last SIPI ICR value  : 0x%llX   (trampoline vector 0x%02llX)\n",
               s, s & 0xFF);
        return 0;
    }

    // "apicdump": read back what the APIC-page write-trap captured (SMP.3b
    // increment 1). Shows the first APIC register write Windows did + the exact
    // instruction bytes, so we can build the write decoder.
    if (argi < argc && _stricmp(argv[argi], "apicdump") == 0) {
        unsigned __int64 n   = hv_call(HV_MAGIC, HVC_APIC_FAULTS, 0);
        unsigned __int64 rip = hv_call(HV_MAGIC, HVC_APIC_RIP, 0);
        unsigned __int64 b0  = hv_call(HV_MAGIC, HVC_APIC_B0, 0);
        unsigned __int64 b1  = hv_call(HV_MAGIC, HVC_APIC_B1, 0);
        unsigned __int64 gpa = hv_call(HV_MAGIC, HVC_APIC_GPA, 0);
        unsigned __int64 i1  = hv_call(HV_MAGIC, HVC_APIC_INFO1, 0);
        unsigned __int64 db = hv_call(HV_MAGIC, HVC_DB_COUNT, 0);
        printf("  APIC-page write faults : %llu\n", n);
        printf("  single-step #DB count  : %llu   (should be ~200 if single-step works)\n", db);
        if (n == 0) {
            printf("  (no APIC write trapped - the page trap may not be firing)\n");
            return 0;
        }
        printf("  first faulting GPA     : 0x%llX  (APIC register 0x%03llX)\n",
               gpa, gpa & 0xFFF);
        printf("  NPF exit_info1         : 0x%llX  (bit1=write:%llu)\n",
               i1, (i1 >> 1) & 1);
        printf("  guest RIP              : 0x%llX\n", rip);
        printf("  instruction bytes      : ");
        for (int i = 0; i < 8; i++) printf("%02llX ", (b0 >> (i * 8)) & 0xFF);
        for (int i = 0; i < 8; i++) printf("%02llX ", (b1 >> (i * 8)) & 0xFF);
        printf("\n");
        // Increment 3: the decoded ICR (AP-startup) writes.
        unsigned __int64 icrlo = hv_call(HV_MAGIC, HVC_ICR_LO, 0);
        unsigned __int64 icrhi = hv_call(HV_MAGIC, HVC_ICR_HI, 0);
        unsigned __int64 icrc  = hv_call(HV_MAGIC, HVC_ICR_COUNT, 0);
        unsigned __int64 initc = hv_call(HV_MAGIC, HVC_INIT_CNT, 0);
        unsigned __int64 sipic = hv_call(HV_MAGIC, HVC_SIPI_CNT, 0);
        unsigned __int64 sipiv = hv_call(HV_MAGIC, HVC_SIPI_VEC, 0);
        unsigned __int64 undec = hv_call(HV_MAGIC, HVC_APIC_UNDEC, 0);
        printf("  --- ICR (AP startup) ---\n");
        printf("  ICR sends / INIT / SIPI: %llu / %llu / %llu   (undecoded: %llu)\n",
               icrc, initc, sipic, undec);
        printf("  last ICR low / high    : 0x%llX / 0x%llX\n", icrlo, icrhi);
        printf("  SIPI trampoline vector : 0x%02llX  -> starts AP at 0x%llX\n",
               sipiv, sipiv << 12);
        unsigned __int64 apup = hv_call(HV_MAGIC, HVC_AP_STARTED, 0);
        unsigned __int64 bx   = hv_call(HV_MAGIC, HVC_AP_BADEXIT, 0);
        unsigned __int64 br   = hv_call(HV_MAGIC, HVC_AP_BADRIP, 0);
        unsigned __int64 bc   = hv_call(HV_MAGIC, HVC_AP_BADCOUNT, 0);
        unsigned __int64 rmx = hv_call(HV_MAGIC, HVC_RM_EXIT, 0);
        unsigned __int64 rmr = hv_call(HV_MAGIC, HVC_RM_RIP, 0);
        unsigned __int64 lps = hv_call(HV_MAGIC, HVC_INIT_LOOPS, 0);
        printf("  APs that ran trampoline: %llu   (as miniSVM guests)\n", apup);
        printf("  AP unexpected exits    : %llu   last code=0x%llX rip=0x%llX\n",
               bc, bx, br);
        printf("  AP real-mode last exit : code=0x%llX rip=0x%llX\n", rmx, rmr);
        printf("  max INIT-handler loops : %llu   (>1 = the reset isn't sticking)\n", lps);
        return 0;
    }

    // Subcommand: find <process-name>
    if (argi + 1 < argc && _stricmp(argv[argi], "find") == 0) {
        const char *name = argv[argi + 1];
        // The hypervisor reads the name string straight out of our address
        // space (it walks our page tables), so we just hand it the pointer.
        unsigned __int64 pid =
            hv_call(HV_MAGIC, HVC_FIND_PID, (unsigned __int64)(ULONG_PTR)name);
        if (pid)
            printf("  %s -> PID %llu   (the hypervisor found this in the\n"
                   "                    kernel's own process list)\n", name, pid);
        else
            printf("  '%s' not found by the hypervisor.\n"
                   "  (either it isn't running, or this Windows build lays out\n"
                   "   EPROCESS differently than the offsets in svm.c)\n", name);
        return pid ? 0 : 2;
    }

    printf("=====================================================\n");
    printf(" miniSVM hypervisor detected BENEATH this Windows! \n");
    printf("=====================================================\n");
    printf("  #VMEXITs serviced   : %llu\n", hv_call(HV_MAGIC, HVC_EXITS, 0));
    printf("  Windows major ver.  : %llu  (the HV read this via VMI)\n",
           hv_call(HV_MAGIC, HVC_WINVER, 0));
    printf("  hypervisor version  : 0x%llX\n", hv_call(HV_MAGIC, HVC_VERSION, 0));
    printf("=====================================================\n");
    printf(" Every line above came from the hypervisor you are\n");
    printf(" running on top of, answered over a VMMCALL channel.\n");
    printf("\n Try:  minictl find explorer.exe\n");
    return 0;
}
