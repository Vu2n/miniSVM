// efi.h - Minimal, self-contained UEFI definitions.
//
// We do NOT depend on EDK2, gnu-efi, or the Windows SDK. Everything the
// hypervisor needs from the firmware is declared here by hand. This keeps the
// repo tiny and lets anyone build it with just MSVC (cl.exe + link.exe) or
// clang. Only the handful of UEFI structures/functions we actually call are
// declared; the many members we don't use are laid out as `void *` placeholders
// so that the byte offsets of the members we DO use are correct.
//
// Reference: UEFI Specification 2.x, "Boot Services" and "System Table".
#pragma once

// ---- Fixed-width integer types (UEFI names) --------------------------------
typedef unsigned char       UINT8;
typedef unsigned short      UINT16;
typedef unsigned int        UINT32;
typedef unsigned long long  UINT64;
typedef signed   long long  INT64;
typedef UINT64              UINTN;    // "native" width; 64-bit on x86-64
typedef unsigned short      CHAR16;   // UTF-16 code unit (L"..." literals)
typedef unsigned char       BOOLEAN;
typedef void                VOID;

typedef UINTN  EFI_STATUS;
typedef VOID  *EFI_HANDLE;

// On the Microsoft x64 ABI (what MSVC emits) the default calling convention
// already matches the UEFI calling convention, so EFIAPI expands to nothing.
// (On System V / clang you would use __attribute__((ms_abi)) here instead.)
#define EFIAPI

#define EFI_SUCCESS  ((EFI_STATUS)0)
// UEFI error codes have the high bit set. e.g. EFI_NOT_FOUND = (high bit | 14).
#define EFI_ERROR(s) (((INT64)(s)) < 0)

#define TRUE  ((BOOLEAN)1)
#define FALSE ((BOOLEAN)0)
#ifndef NULL
#define NULL ((VOID *)0)
#endif

// Freestanding memset/memcpy (defined in main.c; the compiler may emit calls).
void *memset(void *dst, int c, UINTN n);
void *memcpy(void *dst, const void *src, UINTN n);

// ---- Simple Text Output Protocol (the console) -----------------------------
struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, BOOLEAN ExtendedVerification);
typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);

typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET  Reset;
    EFI_TEXT_STRING OutputString;   // <- the only method we use
    // Remaining methods (TestString, QueryMode, SetMode, ...) omitted.
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

// ---- Boot Services (subset) ------------------------------------------------
typedef struct {
    UINT64 Signature;
    UINT32 Revision;
    UINT32 HeaderSize;
    UINT32 CRC32;
    UINT32 Reserved;
} EFI_TABLE_HEADER;

// AllocatePages "Type" values.
#define AllocateAnyPages 0
// MemoryType values.
#define EfiLoaderData            2
#define EfiRuntimeServicesData   6   // preserved by the OS across ExitBootServices

typedef struct {
    UINT32 Data1; UINT16 Data2; UINT16 Data3; UINT8 Data4[8];
} EFI_GUID;

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    UINTN Type, UINTN MemoryType, UINTN Pages, UINT64 *Memory);
typedef EFI_STATUS (EFIAPI *EFI_STALL)(UINTN Microseconds);
typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    UINTN *MemoryMapSize, VOID *MemoryMap, UINTN *MapKey,
    UINTN *DescriptorSize, UINT32 *DescriptorVersion);
typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE ImageHandle, UINTN MapKey);
typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE)(
    UINTN SearchType, EFI_GUID *Protocol, VOID *SearchKey,
    UINTN *BufferSize, EFI_HANDLE *Buffer);
typedef EFI_STATUS (EFIAPI *EFI_LOAD_IMAGE)(
    BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle, VOID *DevicePath,
    VOID *SourceBuffer, UINTN SourceSize, EFI_HANDLE *ImageHandle);
typedef EFI_STATUS (EFIAPI *EFI_START_IMAGE)(
    EFI_HANDLE ImageHandle, UINTN *ExitDataSize, CHAR16 **ExitData);
#define ByProtocol 2

// The Boot Services table. Members before the ones we use are declared as
// void* so the layout (and therefore the offsets) match the spec exactly.
typedef struct {
    EFI_TABLE_HEADER   Hdr;
    VOID *RaiseTPL;
    VOID *RestoreTPL;
    EFI_ALLOCATE_PAGES AllocatePages;   // Memory Services
    VOID *FreePages;
    EFI_GET_MEMORY_MAP GetMemoryMap;
    VOID *AllocatePool;
    VOID *FreePool;
    VOID *CreateEvent;                  // Event & Timer Services
    VOID *SetTimer;
    VOID *WaitForEvent;
    VOID *SignalEvent;
    VOID *CloseEvent;
    VOID *CheckEvent;
    VOID *InstallProtocolInterface;     // Protocol Handler Services
    VOID *ReinstallProtocolInterface;
    VOID *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                        VOID **Interface);
    VOID *Reserved;
    VOID *RegisterProtocolNotify;
    EFI_LOCATE_HANDLE LocateHandle;
    VOID *LocateDevicePath;
    VOID *InstallConfigurationTable;
    EFI_LOAD_IMAGE LoadImage;           // Image Services
    EFI_START_IMAGE StartImage;
    VOID *Exit;
    VOID *UnloadImage;
    EFI_EXIT_BOOT_SERVICES ExitBootServices;
    VOID *GetNextMonotonicCount;        // Misc Services
    EFI_STALL Stall;                    // <- used to pause between messages
    // Remaining members omitted.
} EFI_BOOT_SERVICES;

// ---- System Table ----------------------------------------------------------
typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32  FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    VOID      *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;    // <- the console
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    VOID              *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;            // <- Stall, AllocatePages
    UINTN  NumberOfTableEntries;
    VOID  *ConfigurationTable;
} EFI_SYSTEM_TABLE;

// ---- Loaded Image Protocol (tells us where our own image lives in memory) --
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E,0x3F,0x00,0xA0,0xC9,0x69,0x72,0x3B } }

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    VOID *SystemTable;
    EFI_HANDLE DeviceHandle;
    VOID *FilePath;
    VOID *Reserved;
    UINT32 LoadOptionsSize;
    VOID *LoadOptions;
    VOID *ImageBase;      // where our image is loaded
    UINT64 ImageSize;     // its size in memory
    UINT32 ImageCodeType;
    UINT32 ImageDataType;
    VOID *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

// ---- Simple File System + File protocols (to find bootmgfw.efi) ------------
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964E5B22, 0x6459, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } }
#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    { 0x9576E91, 0x6D3F, 0x11D2, { 0x8E,0x39,0x00,0xA0,0xC9,0x69,0x72,0x3B } }
#define EFI_FILE_MODE_READ 0x0000000000000001ull

typedef struct _EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct _EFI_FILE_PROTOCOL *This,
        struct _EFI_FILE_PROTOCOL **NewHandle, CHAR16 *FileName,
        UINT64 OpenMode, UINT64 Attributes);
    EFI_STATUS (EFIAPI *Close)(struct _EFI_FILE_PROTOCOL *This);
    // remaining members (Delete, Read, Write, ...) omitted - not used
} EFI_FILE_PROTOCOL;

typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
        EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

// A device-path node header (Type/SubType/Length). We build one file-path node.
typedef struct { UINT8 Type; UINT8 SubType; UINT8 Length[2]; } EFI_DEV_PATH_HDR;
