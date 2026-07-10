// cpu.h - Thin wrappers over the CPU control instructions we need.
//
// Rather than #include <intrin.h> (which can pull in CRT machinery) we declare
// exactly the MSVC compiler intrinsics we use and mark them #pragma intrinsic.
// These compile down to single instructions (cpuid, rdmsr/wrmsr, mov cr, ...).
#pragma once
#include "efi.h"

// --- Raw MSVC intrinsics (implemented by the compiler as inline instructions).
unsigned __int64 __readmsr(unsigned long Register);
void             __writemsr(unsigned long Register, unsigned __int64 Value);
void             __cpuidex(int CpuInfo[4], int Leaf, int Subleaf);
void             __cpuid(int CpuInfo[4], int Leaf);
unsigned __int64 __readcr0(void);
unsigned __int64 __readcr3(void);
unsigned __int64 __readcr4(void);
void             __writecr3(unsigned __int64 Value);
void             __writecr4(unsigned __int64 Value);
void             _disable(void);   // cli
void             __halt(void);     // hlt
unsigned char    __inbyte(unsigned short Port);
void             __outbyte(unsigned short Port, unsigned char Data);
#pragma intrinsic(__readmsr, __writemsr, __cpuidex, __cpuid)
#pragma intrinsic(__readcr0, __readcr3, __readcr4, __writecr3, __writecr4)
#pragma intrinsic(_disable, __halt, __inbyte, __outbyte)

// --- Model Specific Registers we touch -------------------------------------
#define MSR_EFER        0xC0000080u   // Extended Feature Enable Register
#define MSR_VM_CR       0xC0010114u   // SVM global control
#define MSR_VM_HSAVE_PA 0xC0010117u   // physical addr of host state-save area

#define EFER_SVME (1ull << 12)        // EFER.SVME: enable SVM instructions
#define VM_CR_SVMDIS (1ull << 4)      // VM_CR.SVMDIS: SVM disabled (BIOS lock)

// --- CPUID leaves ----------------------------------------------------------
#define CPUID_EXT_FEATURES 0x80000001u // ECX bit 2 (SVM) tells us AMD-V exists
#define CPUID_SVM_FEATURES 0x8000000Au // EDX = SVM feature flags, EBX = #ASIDs

// CR4.OSFXSR (bit 9) / OSXMMEXCPT (bit 10): must be set before any SSE
// instruction executes. MSVC's x64 codegen assumes SSE2 is available, and some
// firmware hands control to us with these cleared, so we set them at startup to
// avoid a spurious #UD.
#define CR4_OSFXSR     (1ull << 9)
#define CR4_OSXMMEXCPT (1ull << 10)

static void cpu_enable_sse(void) {
    __writecr4(__readcr4() | CR4_OSFXSR | CR4_OSXMMEXCPT);
}
