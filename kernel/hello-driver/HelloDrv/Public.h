/*++

Module Name:

    Public.h

Abstract:

    Shared definitions between the HelloDrv kernel driver and any user-mode
    consumer (HelloDrvMonitor.exe and friends).

    This header is INCLUDED FROM BOTH KERNEL AND USER MODE. It must therefore
    only depend on types that are defined identically in both worlds:
    fixed-width primitives, LARGE_INTEGER, BOOLEAN, USHORT, ULONG, WCHAR.

    Pre-include rules:
      * Kernel:  #include <ntddk.h>   (or <wdm.h>)  before this header.
      * User:    #include <windows.h> and <winioctl.h> before this header.

Environment:

    Kernel mode and user mode.

License:

    MIT-style; educational sample.

--*/

#pragma once

//
// Device names.
//
//   * In the kernel we create the device with HELLODRV_NT_DEVICE_NAME and a
//     symbolic link in the DOS device namespace at HELLODRV_DOS_DEVICE_NAME.
//   * From user mode you open the device by its Win32 path
//     HELLODRV_USER_DEVICE_PATH (which the OS resolves to the symbolic link
//     and through it to the NT device).
//
#define HELLODRV_NT_DEVICE_NAME       L"\\Device\\HelloDrv"
#define HELLODRV_DOS_DEVICE_NAME      L"\\DosDevices\\HelloDrv"
#define HELLODRV_USER_DEVICE_PATH     L"\\\\.\\HelloDrv"

//
// Custom device type. Microsoft reserves device types 0..0x7FFF; values in
// the range 0x8000..0xFFFF are available for third-party drivers.
//
#define FILE_DEVICE_HELLODRV          0x8000u

//
// IOCTL codes.
//
//   IOCTL_HELLODRV_GET_NEXT_EVENT
//     Caller-allocated output buffer of at least sizeof(HELLODRV_EVENT).
//     The driver completes the request as soon as the next event is
//     available. If no event is queued at call time, the request is parked
//     on a manual queue and completed when the next process / image /
//     thread notification fires (the "inverted call" pattern). Use a worker
//     thread or an APC-friendly wait if you don't want to block your UI.
//
//   IOCTL_HELLODRV_GET_STATS
//     Caller-allocated output buffer of sizeof(HELLODRV_STATS). Returns
//     counters since driver load (or since the last RESET_STATS).
//
//   IOCTL_HELLODRV_RESET_STATS
//     No input or output. Zeros all counters.
//
#define IOCTL_HELLODRV_GET_NEXT_EVENT \
    CTL_CODE(FILE_DEVICE_HELLODRV, 0x800, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_HELLODRV_GET_STATS \
    CTL_CODE(FILE_DEVICE_HELLODRV, 0x801, METHOD_BUFFERED, FILE_READ_DATA)

#define IOCTL_HELLODRV_RESET_STATS \
    CTL_CODE(FILE_DEVICE_HELLODRV, 0x802, METHOD_BUFFERED, FILE_WRITE_DATA)

//
// Event payload.
//
//   * Image / command-line strings are inlined as fixed-size wide buffers
//     and length counts, NUL-terminated where possible. This keeps the
//     struct flat (no embedded pointers), which is essential for IOCTL
//     marshalling.
//   * Lengths are character counts (not bytes), excluding the trailing NUL.
//   * For events that don't have a particular field (e.g. a thread-exit
//     event has no command line), the field is zeroed.
//
#define HELLODRV_MAX_PATH_CCH         260
#define HELLODRV_MAX_CMDLINE_CCH      1024

typedef enum _HELLODRV_EVENT_TYPE {
    HelloDrvEventNone           = 0,
    HelloDrvEventProcessCreate  = 1,
    HelloDrvEventProcessExit    = 2,
    HelloDrvEventImageLoad      = 3,
    HelloDrvEventThreadCreate   = 4,
    HelloDrvEventThreadExit     = 5,
} HELLODRV_EVENT_TYPE;

#include <pshpack8.h>
typedef struct _HELLODRV_EVENT {
    ULONG               Size;             // sizeof(HELLODRV_EVENT) -- versioning hint.
    ULONG               Type;             // HELLODRV_EVENT_TYPE.
    LARGE_INTEGER       Timestamp;        // 100-ns intervals since 1601-01-01 UTC.
    ULONG               ProcessId;
    ULONG               ParentProcessId;  // 0 if not applicable.
    ULONG               ThreadId;         // 0 if not applicable.
    ULONG               KernelInitiated;  // BOOLEAN promoted to ULONG for alignment.
    USHORT              ImageFileNameCch; // 0 if no image path.
    USHORT              CommandLineCch;   // 0 if no command line.
    ULONG               Reserved;
    WCHAR               ImageFileName[HELLODRV_MAX_PATH_CCH];
    WCHAR               CommandLine[HELLODRV_MAX_CMDLINE_CCH];
} HELLODRV_EVENT, *PHELLODRV_EVENT;
#include <poppack.h>

typedef struct _HELLODRV_STATS {
    ULONG Size;                            // sizeof(HELLODRV_STATS).
    ULONG ProcessCreates;
    ULONG ProcessExits;
    ULONG ImageLoads;
    ULONG ThreadCreates;
    ULONG ThreadExits;
    ULONG EventsDelivered;
    ULONG EventsDroppedDueToBackpressure;  // Ring buffer overflow.
    ULONG EventsDroppedDueToAllocFailure;  // ExAllocatePool2 returned NULL.
    ULONG PendingRequests;                 // User-mode IOCTLs currently parked.
} HELLODRV_STATS, *PHELLODRV_STATS;
