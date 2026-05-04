/*++

Module Name:

    Driver.h

Abstract:

    Internal declarations for the HelloDrv KMDF driver. Anything that
    crosses the kernel/user-mode boundary lives in Public.h instead.

Environment:

    Kernel mode (KMDF).

--*/

#pragma once

#include <ntddk.h>
#include <wdf.h>

#include "Public.h"

//
// Pool tag visible to !poolused / poolmon.
//
#define HELLODRV_POOL_TAG               'vrDH'

//
// Bounded kernel-side queue depth. If user-mode falls behind, the oldest
// queued events are dropped and Stats.EventsDroppedDueToBackpressure is
// incremented. Keep this small enough that we don't tie up too much
// non-paged pool, large enough to absorb short bursts (e.g. a build that
// spawns hundreds of cl.exe / link.exe instances per second).
//
#define HELLODRV_MAX_QUEUED_EVENTS      512

//
// Per-event linked-list node living on non-paged pool.
//
typedef struct _HELLODRV_QUEUED_EVENT {
    LIST_ENTRY     ListEntry;
    HELLODRV_EVENT Event;
} HELLODRV_QUEUED_EVENT, *PHELLODRV_QUEUED_EVENT;

//
// Driver-wide state. Exactly one instance, populated in DriverEntry.
//
typedef struct _HELLODRV_GLOBALS {
    WDFDRIVER       WdfDriver;
    WDFDEVICE       ControlDevice;
    WDFQUEUE        DefaultQueue;       // EvtIoDeviceControl.
    WDFQUEUE        PendingRequests;    // Manual queue for inverted calls.

    KSPIN_LOCK      EventListLock;      // Protects EventListHead/Count.
    LIST_ENTRY      EventListHead;
    ULONG           EventListCount;

    HELLODRV_STATS  Stats;              // Updated under EventListLock.
    KSPIN_LOCK      StatsLock;          // For non-event-path stat updates.

    LONG            CallbackRegistrationCount;  // Bitmask of which Ps* APIs registered.
} HELLODRV_GLOBALS;

#define HELLODRV_REG_PROCESS  0x1
#define HELLODRV_REG_IMAGE    0x2
#define HELLODRV_REG_THREAD   0x4

//
// Logging macros. DPFLTR_IHVDRIVER_ID is visible at INFO level in DebugView
// when "Capture Kernel" + "Enable Verbose Kernel Output" are on.
//
#define HELLODRV_LOG_INFO(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,    "[HelloDrv] " fmt, __VA_ARGS__)
#define HELLODRV_LOG_WARN(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_WARNING_LEVEL, "[HelloDrv] WARN: " fmt, __VA_ARGS__)
#define HELLODRV_LOG_ERROR(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL,   "[HelloDrv] ERR : " fmt, __VA_ARGS__)

//
// Forward declarations.
//
DRIVER_INITIALIZE        DriverEntry;
EVT_WDF_DRIVER_UNLOAD    HelloDrvEvtDriverUnload;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL HelloDrvEvtIoDeviceControl;

//
// Notify-routine prototypes.
//
VOID
HelloDrvProcessNotify(
    _Inout_     PEPROCESS              Process,
    _In_        HANDLE                 ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    );

VOID
HelloDrvImageNotify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_     HANDLE          ProcessId,
    _In_     PIMAGE_INFO     ImageInfo
    );

VOID
HelloDrvThreadNotify(
    _In_ HANDLE  ProcessId,
    _In_ HANDLE  ThreadId,
    _In_ BOOLEAN Create
    );

//
// Helpers.
//
PHELLODRV_QUEUED_EVENT
HelloDrvAllocEvent(
    _In_ HELLODRV_EVENT_TYPE Type
    );

VOID
HelloDrvFreeEvent(
    _In_ PHELLODRV_QUEUED_EVENT QueuedEvent
    );

VOID
HelloDrvDispatchEvent(
    _In_ PHELLODRV_QUEUED_EVENT QueuedEvent
    );

VOID
HelloDrvCopyUnicodeStringField(
    _Out_writes_(MaxCch) PWCHAR Dest,
    _In_                 USHORT MaxCch,
    _Out_                PUSHORT OutCch,
    _In_opt_             PCUNICODE_STRING Source
    );

extern HELLODRV_GLOBALS g_HelloDrv;
