/*++

Module Name:

    Driver.c

Abstract:

    HelloDrv -- a minimal but feature-complete KMDF kernel driver that:

      1. Registers process create/exit, image-load, and thread create/exit
         notifications via the documented Ps* notification APIs.
      2. Exposes a control device (\Device\HelloDrv, \\.\HelloDrv) with an
         IOCTL interface so a user-mode subscriber can pull events live
         (inverted-call pattern).
      3. Buffers events in a bounded non-paged-pool ring so that short
         bursts (e.g. a noisy build) don't drop traffic. Counters are
         exposed via IOCTL_HELLODRV_GET_STATS.

    What this driver intentionally does NOT do:

      * It does not read another process's memory. There is no
        MmCopyVirtualMemory / NtReadVirtualMemory wrapper, no IOCTL to
        peek at user-mode pages, no NtUserBuildHwndList shenanigans.
      * It does not hide its own image, service, or any other process.
        It is fully visible in `sc query`, `driverquery`, the loaded-
        modules list, etc.
      * It does not unhook, patch, or interfere with anti-cheat,
        anti-virus, or anti-malware components.

    The driver is purely observational. It is a learning sample.

Environment:

    Kernel mode (KMDF). Tested on Windows 10 22H2 / Windows 11 24H2 x64
    with WDK 10.0.26100.

License:

    MIT-style. Public domain for learning. No warranty.

--*/

#include "Driver.h"

HELLODRV_GLOBALS g_HelloDrv = {0};

//
// SDDL: only LocalSystem and Administrators may open the device. This
// matches the threat model of a debugging / monitoring tool.
//
DECLARE_CONST_UNICODE_STRING(
    HelloDrvSddlString,
    L"D:P(A;;GA;;;SY)(A;;GA;;;BA)");

//============================================================================
// DriverEntry / Unload
//============================================================================

NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    NTSTATUS                  status;
    WDF_DRIVER_CONFIG         driverConfig;
    PWDFDEVICE_INIT           deviceInit = NULL;
    UNICODE_STRING            ntDeviceName;
    UNICODE_STRING            dosDeviceName;
    WDF_OBJECT_ATTRIBUTES     deviceAttrs;
    WDF_IO_QUEUE_CONFIG       defaultQueueConfig;
    WDF_IO_QUEUE_CONFIG       manualQueueConfig;

    HELLODRV_LOG_INFO("DriverEntry; RegistryPath=%wZ\n", RegistryPath);

    //
    // Initialize globals.
    //
    KeInitializeSpinLock(&g_HelloDrv.EventListLock);
    KeInitializeSpinLock(&g_HelloDrv.StatsLock);
    InitializeListHead(&g_HelloDrv.EventListHead);
    g_HelloDrv.EventListCount = 0;
    g_HelloDrv.Stats.Size = sizeof(HELLODRV_STATS);

    //
    // Create the WDFDRIVER. Software-only, non-PnP.
    //
    WDF_DRIVER_CONFIG_INIT(&driverConfig, WDF_NO_EVENT_CALLBACK);
    driverConfig.DriverInitFlags = WdfDriverInitNonPnpDriver;
    driverConfig.EvtDriverUnload = HelloDrvEvtDriverUnload;

    status = WdfDriverCreate(DriverObject,
                             RegistryPath,
                             WDF_NO_OBJECT_ATTRIBUTES,
                             &driverConfig,
                             &g_HelloDrv.WdfDriver);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfDriverCreate failed: 0x%08X\n", status);
        return status;
    }

    //
    // Allocate a control-device init block. Control devices are KMDF's way
    // of saying "I'm not a PnP device, I just want a /Device/Foo with an
    // IOCTL surface".
    //
    deviceInit = WdfControlDeviceInitAllocate(g_HelloDrv.WdfDriver,
                                              &HelloDrvSddlString);
    if (deviceInit == NULL) {
        HELLODRV_LOG_ERROR("WdfControlDeviceInitAllocate returned NULL\n");
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    //
    // Name the device, set buffered IO so the framework manages the
    // input/output buffers for us.
    //
    RtlInitUnicodeString(&ntDeviceName, HELLODRV_NT_DEVICE_NAME);
    status = WdfDeviceInitAssignName(deviceInit, &ntDeviceName);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfDeviceInitAssignName failed: 0x%08X\n", status);
        WdfDeviceInitFree(deviceInit);
        return status;
    }

    WdfDeviceInitSetIoType(deviceInit, WdfDeviceIoBuffered);
    WdfDeviceInitSetExclusive(deviceInit, FALSE);
    WdfDeviceInitSetCharacteristics(deviceInit, FILE_DEVICE_SECURE_OPEN, FALSE);

    WDF_OBJECT_ATTRIBUTES_INIT(&deviceAttrs);

    status = WdfDeviceCreate(&deviceInit, &deviceAttrs, &g_HelloDrv.ControlDevice);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfDeviceCreate failed: 0x%08X\n", status);
        if (deviceInit != NULL) {
            WdfDeviceInitFree(deviceInit);
        }
        return status;
    }

    //
    // Symbolic link so user-mode can CreateFile(L"\\\\.\\HelloDrv").
    //
    RtlInitUnicodeString(&dosDeviceName, HELLODRV_DOS_DEVICE_NAME);
    status = WdfDeviceCreateSymbolicLink(g_HelloDrv.ControlDevice, &dosDeviceName);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfDeviceCreateSymbolicLink failed: 0x%08X\n", status);
        return status;
    }

    //
    // Default IOCTL queue. Sequential dispatch so the driver only sees one
    // IOCTL at a time -- avoids needing extra locking inside the dispatch
    // routine.
    //
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&defaultQueueConfig,
                                           WdfIoQueueDispatchSequential);
    defaultQueueConfig.EvtIoDeviceControl = HelloDrvEvtIoDeviceControl;

    status = WdfIoQueueCreate(g_HelloDrv.ControlDevice,
                              &defaultQueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &g_HelloDrv.DefaultQueue);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfIoQueueCreate(default) failed: 0x%08X\n", status);
        return status;
    }

    //
    // Manual queue: holds GET_NEXT_EVENT requests that arrived while no
    // event was queued. We pull them out and complete them as events fire.
    //
    WDF_IO_QUEUE_CONFIG_INIT(&manualQueueConfig, WdfIoQueueDispatchManual);

    status = WdfIoQueueCreate(g_HelloDrv.ControlDevice,
                              &manualQueueConfig,
                              WDF_NO_OBJECT_ATTRIBUTES,
                              &g_HelloDrv.PendingRequests);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfIoQueueCreate(manual) failed: 0x%08X\n", status);
        return status;
    }

    //
    // Register the kernel callbacks. We do this last so the device surface
    // is fully ready before events can fire.
    //
    status = PsSetCreateProcessNotifyRoutineEx(HelloDrvProcessNotify, FALSE);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("PsSetCreateProcessNotifyRoutineEx failed: 0x%08X "
                           "(missing /INTEGRITYCHECK?)\n", status);
        return status;
    }
    g_HelloDrv.CallbackRegistrationCount |= HELLODRV_REG_PROCESS;

    status = PsSetLoadImageNotifyRoutine(HelloDrvImageNotify);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("PsSetLoadImageNotifyRoutine failed: 0x%08X\n", status);
        // Continue without image notifications; not fatal.
    } else {
        g_HelloDrv.CallbackRegistrationCount |= HELLODRV_REG_IMAGE;
    }

    status = PsSetCreateThreadNotifyRoutine(HelloDrvThreadNotify);
    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("PsSetCreateThreadNotifyRoutine failed: 0x%08X\n", status);
        // Continue without thread notifications; not fatal.
    } else {
        g_HelloDrv.CallbackRegistrationCount |= HELLODRV_REG_THREAD;
    }

    //
    // Tell KMDF that the control device is fully constructed. Until this is
    // called the device is not visible to user-mode opens.
    //
    WdfControlFinishInitializing(g_HelloDrv.ControlDevice);

    HELLODRV_LOG_INFO("Loaded. Callbacks registered: 0x%x. "
                      "Open \"\\\\.\\HelloDrv\" from user mode to subscribe.\n",
                      g_HelloDrv.CallbackRegistrationCount);
    return STATUS_SUCCESS;
}

VOID
HelloDrvEvtDriverUnload(
    _In_ WDFDRIVER Driver
    )
{
    NTSTATUS    status;
    KIRQL       oldIrql;
    LIST_ENTRY* entry;

    UNREFERENCED_PARAMETER(Driver);

    HELLODRV_LOG_INFO("Unload requested. Tearing down...\n");

    //
    // Unregister callbacks first so no new events can be queued or
    // dispatched while we drain the queues.
    //
    if (g_HelloDrv.CallbackRegistrationCount & HELLODRV_REG_THREAD) {
        status = PsRemoveCreateThreadNotifyRoutine(HelloDrvThreadNotify);
        if (!NT_SUCCESS(status)) {
            HELLODRV_LOG_ERROR("PsRemoveCreateThreadNotifyRoutine failed: 0x%08X\n", status);
        }
    }
    if (g_HelloDrv.CallbackRegistrationCount & HELLODRV_REG_IMAGE) {
        status = PsRemoveLoadImageNotifyRoutine(HelloDrvImageNotify);
        if (!NT_SUCCESS(status)) {
            HELLODRV_LOG_ERROR("PsRemoveLoadImageNotifyRoutine failed: 0x%08X\n", status);
        }
    }
    if (g_HelloDrv.CallbackRegistrationCount & HELLODRV_REG_PROCESS) {
        status = PsSetCreateProcessNotifyRoutineEx(HelloDrvProcessNotify, TRUE);
        if (!NT_SUCCESS(status)) {
            HELLODRV_LOG_ERROR("PsSet...ProcessNotify(remove) failed: 0x%08X\n", status);
        }
    }

    //
    // Cancel any user-mode IOCTLs still parked in the manual queue.
    //
    if (g_HelloDrv.PendingRequests != NULL) {
        WdfIoQueuePurgeSynchronously(g_HelloDrv.PendingRequests);
    }

    //
    // Drain any events still in the kernel-side ring.
    //
    KeAcquireSpinLock(&g_HelloDrv.EventListLock, &oldIrql);
    while (!IsListEmpty(&g_HelloDrv.EventListHead)) {
        entry = RemoveHeadList(&g_HelloDrv.EventListHead);
        KeReleaseSpinLock(&g_HelloDrv.EventListLock, oldIrql);
        HelloDrvFreeEvent(CONTAINING_RECORD(entry, HELLODRV_QUEUED_EVENT, ListEntry));
        KeAcquireSpinLock(&g_HelloDrv.EventListLock, &oldIrql);
    }
    g_HelloDrv.EventListCount = 0;
    KeReleaseSpinLock(&g_HelloDrv.EventListLock, oldIrql);

    HELLODRV_LOG_INFO("Unloaded.\n");
}

//============================================================================
// Event allocation, dispatch, and bookkeeping.
//============================================================================

PHELLODRV_QUEUED_EVENT
HelloDrvAllocEvent(
    _In_ HELLODRV_EVENT_TYPE Type
    )
{
    PHELLODRV_QUEUED_EVENT q;

    q = (PHELLODRV_QUEUED_EVENT)ExAllocatePool2(
        POOL_FLAG_NON_PAGED,
        sizeof(*q),
        HELLODRV_POOL_TAG);

    if (q == NULL) {
        KIRQL oldIrql;
        KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
        g_HelloDrv.Stats.EventsDroppedDueToAllocFailure++;
        KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);
        return NULL;
    }

    q->Event.Size = sizeof(HELLODRV_EVENT);
    q->Event.Type = (ULONG)Type;
    KeQuerySystemTimePrecise(&q->Event.Timestamp);
    return q;
}

VOID
HelloDrvFreeEvent(
    _In_ PHELLODRV_QUEUED_EVENT QueuedEvent
    )
{
    if (QueuedEvent != NULL) {
        ExFreePoolWithTag(QueuedEvent, HELLODRV_POOL_TAG);
    }
}

VOID
HelloDrvCopyUnicodeStringField(
    _Out_writes_(MaxCch) PWCHAR Dest,
    _In_                 USHORT MaxCch,
    _Out_                PUSHORT OutCch,
    _In_opt_             PCUNICODE_STRING Source
    )
{
    USHORT cch;

    *OutCch = 0;
    if (MaxCch == 0 || Dest == NULL) {
        return;
    }
    Dest[0] = L'\0';

    if (Source == NULL || Source->Buffer == NULL || Source->Length == 0) {
        return;
    }

    cch = (USHORT)(Source->Length / sizeof(WCHAR));
    if (cch >= MaxCch) {
        cch = (USHORT)(MaxCch - 1);
    }

    RtlCopyMemory(Dest, Source->Buffer, cch * sizeof(WCHAR));
    Dest[cch] = L'\0';
    *OutCch = cch;
}

//
// HelloDrvDispatchEvent: try to hand the event to a waiting user-mode IOCTL;
// failing that, queue it on the kernel-side ring (dropping the oldest if
// we're full).
//
VOID
HelloDrvDispatchEvent(
    _In_ PHELLODRV_QUEUED_EVENT QueuedEvent
    )
{
    NTSTATUS    status;
    WDFREQUEST  request = NULL;
    PVOID       outBuffer;
    size_t      outBufferLen;
    KIRQL       oldIrql;

    //
    // Fast path: a user-mode IOCTL is parked waiting for an event.
    //
    status = WdfIoQueueRetrieveNextRequest(g_HelloDrv.PendingRequests, &request);
    if (NT_SUCCESS(status) && request != NULL) {
        status = WdfRequestRetrieveOutputBuffer(request,
                                                sizeof(HELLODRV_EVENT),
                                                &outBuffer,
                                                &outBufferLen);
        if (NT_SUCCESS(status) && outBufferLen >= sizeof(HELLODRV_EVENT)) {
            RtlCopyMemory(outBuffer, &QueuedEvent->Event, sizeof(HELLODRV_EVENT));
            WdfRequestCompleteWithInformation(request, STATUS_SUCCESS,
                                              sizeof(HELLODRV_EVENT));

            KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
            g_HelloDrv.Stats.EventsDelivered++;
            KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);

            HelloDrvFreeEvent(QueuedEvent);
            return;
        }

        // Output buffer was too small or missing; complete the request with
        // an error and fall through to enqueue the event for the next
        // caller.
        WdfRequestComplete(request, STATUS_BUFFER_TOO_SMALL);
    }

    //
    // Slow path: park the event in the ring buffer.
    //
    KeAcquireSpinLock(&g_HelloDrv.EventListLock, &oldIrql);

    if (g_HelloDrv.EventListCount >= HELLODRV_MAX_QUEUED_EVENTS) {
        // Drop the oldest event to make room (FIFO semantics).
        LIST_ENTRY* oldest = RemoveHeadList(&g_HelloDrv.EventListHead);
        g_HelloDrv.EventListCount--;
        g_HelloDrv.Stats.EventsDroppedDueToBackpressure++;
        KeReleaseSpinLock(&g_HelloDrv.EventListLock, oldIrql);

        HelloDrvFreeEvent(CONTAINING_RECORD(oldest, HELLODRV_QUEUED_EVENT, ListEntry));

        KeAcquireSpinLock(&g_HelloDrv.EventListLock, &oldIrql);
    }

    InsertTailList(&g_HelloDrv.EventListHead, &QueuedEvent->ListEntry);
    g_HelloDrv.EventListCount++;
    KeReleaseSpinLock(&g_HelloDrv.EventListLock, oldIrql);
}

//============================================================================
// Notify routines (PASSIVE_LEVEL).
//============================================================================

VOID
HelloDrvProcessNotify(
    _Inout_     PEPROCESS              Process,
    _In_        HANDLE                 ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    PHELLODRV_QUEUED_EVENT q;
    KIRQL                  oldIrql;

    UNREFERENCED_PARAMETER(Process);

    if (CreateInfo != NULL) {
        q = HelloDrvAllocEvent(HelloDrvEventProcessCreate);
        if (q == NULL) return;

        q->Event.ProcessId       = HandleToUlong(ProcessId);
        q->Event.ParentProcessId = HandleToUlong(CreateInfo->ParentProcessId);
        q->Event.ThreadId        = 0;
        q->Event.KernelInitiated = 0;

        HelloDrvCopyUnicodeStringField(q->Event.ImageFileName,
                                       HELLODRV_MAX_PATH_CCH,
                                       &q->Event.ImageFileNameCch,
                                       CreateInfo->ImageFileName);
        HelloDrvCopyUnicodeStringField(q->Event.CommandLine,
                                       HELLODRV_MAX_CMDLINE_CCH,
                                       &q->Event.CommandLineCch,
                                       CreateInfo->CommandLine);

        KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
        g_HelloDrv.Stats.ProcessCreates++;
        KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);
    } else {
        q = HelloDrvAllocEvent(HelloDrvEventProcessExit);
        if (q == NULL) return;

        q->Event.ProcessId       = HandleToUlong(ProcessId);
        q->Event.ParentProcessId = 0;
        q->Event.ThreadId        = 0;
        q->Event.KernelInitiated = 0;
        q->Event.ImageFileNameCch = 0;
        q->Event.ImageFileName[0] = L'\0';
        q->Event.CommandLineCch  = 0;
        q->Event.CommandLine[0]  = L'\0';

        KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
        g_HelloDrv.Stats.ProcessExits++;
        KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);
    }

    HelloDrvDispatchEvent(q);
}

VOID
HelloDrvImageNotify(
    _In_opt_ PUNICODE_STRING FullImageName,
    _In_     HANDLE          ProcessId,
    _In_     PIMAGE_INFO     ImageInfo
    )
{
    PHELLODRV_QUEUED_EVENT q;
    KIRQL                  oldIrql;

    q = HelloDrvAllocEvent(HelloDrvEventImageLoad);
    if (q == NULL) return;

    q->Event.ProcessId       = HandleToUlong(ProcessId);
    q->Event.ParentProcessId = 0;
    q->Event.ThreadId        = 0;
    q->Event.KernelInitiated = (ImageInfo != NULL && ImageInfo->SystemModeImage) ? 1 : 0;

    HelloDrvCopyUnicodeStringField(q->Event.ImageFileName,
                                   HELLODRV_MAX_PATH_CCH,
                                   &q->Event.ImageFileNameCch,
                                   FullImageName);
    q->Event.CommandLineCch = 0;
    q->Event.CommandLine[0] = L'\0';

    KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
    g_HelloDrv.Stats.ImageLoads++;
    KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);

    HelloDrvDispatchEvent(q);
}

VOID
HelloDrvThreadNotify(
    _In_ HANDLE  ProcessId,
    _In_ HANDLE  ThreadId,
    _In_ BOOLEAN Create
    )
{
    PHELLODRV_QUEUED_EVENT q;
    KIRQL                  oldIrql;

    q = HelloDrvAllocEvent(Create ? HelloDrvEventThreadCreate
                                  : HelloDrvEventThreadExit);
    if (q == NULL) return;

    q->Event.ProcessId        = HandleToUlong(ProcessId);
    q->Event.ParentProcessId  = 0;
    q->Event.ThreadId         = HandleToUlong(ThreadId);
    q->Event.KernelInitiated  = 0;
    q->Event.ImageFileNameCch = 0;
    q->Event.ImageFileName[0] = L'\0';
    q->Event.CommandLineCch   = 0;
    q->Event.CommandLine[0]   = L'\0';

    KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
    if (Create) {
        g_HelloDrv.Stats.ThreadCreates++;
    } else {
        g_HelloDrv.Stats.ThreadExits++;
    }
    KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);

    HelloDrvDispatchEvent(q);
}

//============================================================================
// EvtIoDeviceControl - the user-mode IOCTL surface.
//============================================================================

VOID
HelloDrvEvtIoDeviceControl(
    _In_ WDFQUEUE   Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t     OutputBufferLength,
    _In_ size_t     InputBufferLength,
    _In_ ULONG      IoControlCode
    )
{
    NTSTATUS                status;
    PVOID                   outBuffer;
    size_t                  outLen;
    KIRQL                   oldIrql;
    PHELLODRV_QUEUED_EVENT  q = NULL;
    PHELLODRV_STATS         statsOut;

    UNREFERENCED_PARAMETER(Queue);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {

    case IOCTL_HELLODRV_GET_NEXT_EVENT:
        if (OutputBufferLength < sizeof(HELLODRV_EVENT)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }

        //
        // Try the kernel-side ring first.
        //
        KeAcquireSpinLock(&g_HelloDrv.EventListLock, &oldIrql);
        if (!IsListEmpty(&g_HelloDrv.EventListHead)) {
            LIST_ENTRY* entry = RemoveHeadList(&g_HelloDrv.EventListHead);
            g_HelloDrv.EventListCount--;
            q = CONTAINING_RECORD(entry, HELLODRV_QUEUED_EVENT, ListEntry);
        }
        KeReleaseSpinLock(&g_HelloDrv.EventListLock, oldIrql);

        if (q != NULL) {
            status = WdfRequestRetrieveOutputBuffer(Request,
                                                    sizeof(HELLODRV_EVENT),
                                                    &outBuffer,
                                                    &outLen);
            if (!NT_SUCCESS(status)) {
                HelloDrvFreeEvent(q);
                WdfRequestComplete(Request, status);
                return;
            }
            RtlCopyMemory(outBuffer, &q->Event, sizeof(HELLODRV_EVENT));
            HelloDrvFreeEvent(q);

            KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
            g_HelloDrv.Stats.EventsDelivered++;
            KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);

            WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                              sizeof(HELLODRV_EVENT));
            return;
        }

        //
        // No queued events. Park the request on the manual queue and let
        // the next notify callback complete it ("inverted call").
        //
        status = WdfRequestForwardToIoQueue(Request, g_HelloDrv.PendingRequests);
        if (!NT_SUCCESS(status)) {
            HELLODRV_LOG_ERROR("WdfRequestForwardToIoQueue failed: 0x%08X\n", status);
            WdfRequestComplete(Request, status);
            return;
        }

        KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
        g_HelloDrv.Stats.PendingRequests++;
        KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);
        return;

    case IOCTL_HELLODRV_GET_STATS:
        if (OutputBufferLength < sizeof(HELLODRV_STATS)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        status = WdfRequestRetrieveOutputBuffer(Request,
                                                sizeof(HELLODRV_STATS),
                                                &outBuffer,
                                                &outLen);
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        statsOut = (PHELLODRV_STATS)outBuffer;

        KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
        RtlCopyMemory(statsOut, &g_HelloDrv.Stats, sizeof(HELLODRV_STATS));
        statsOut->Size = sizeof(HELLODRV_STATS);
        KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);

        WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS,
                                          sizeof(HELLODRV_STATS));
        return;

    case IOCTL_HELLODRV_RESET_STATS:
        KeAcquireSpinLock(&g_HelloDrv.StatsLock, &oldIrql);
        RtlZeroMemory(&g_HelloDrv.Stats, sizeof(HELLODRV_STATS));
        g_HelloDrv.Stats.Size = sizeof(HELLODRV_STATS);
        KeReleaseSpinLock(&g_HelloDrv.StatsLock, oldIrql);
        WdfRequestComplete(Request, STATUS_SUCCESS);
        return;

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}
