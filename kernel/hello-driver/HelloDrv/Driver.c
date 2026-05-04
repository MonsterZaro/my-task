/*++

Module Name:

    Driver.c

Abstract:

    Minimal KMDF "Hello, kernel" sample.

    Registers a process create/exit callback via
    PsSetCreateProcessNotifyRoutineEx and logs each event with DbgPrintEx so
    they show up in DebugView (with "Capture Kernel" enabled) or in WinDbg.

    The driver is a non-PnP, software-only KMDF driver. It has no device
    object and no I/O dispatch, so it is installed as a kernel service via
    `sc create` rather than via an INF.

Environment:

    Kernel mode (KMDF). Tested on Windows 10 / 11 x64 with WDK 10.0.26100.

License:

    MIT-style: do whatever you want, no warranty. Educational sample.

--*/

#include <ntddk.h>
#include <wdf.h>

//
// Forward declarations.
//
DRIVER_INITIALIZE         DriverEntry;
EVT_WDF_DRIVER_UNLOAD     HelloDrvEvtDriverUnload;

static VOID
HelloDrvProcessNotify(
    _Inout_     PEPROCESS              Process,
    _In_        HANDLE                 ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
);

//
// Convenience print macro. DPFLTR_IHVDRIVER_ID is a generic 3rd-party
// component ID that does not require any registry mask configuration to
// appear in DebugView at INFO level on a checked build, and is visible at
// ERROR level on a free build by default.
//
#define HELLODRV_TAG            'vrDH'
#define HELLODRV_LOG_INFO(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_INFO_LEVEL,  "[HelloDrv] " fmt, __VA_ARGS__)
#define HELLODRV_LOG_ERROR(fmt, ...) \
    DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[HelloDrv] " fmt, __VA_ARGS__)

//
// DriverEntry: KMDF entry point. Registers the framework driver object and
// then registers our process-notify callback.
//
NTSTATUS
DriverEntry(
    _In_ PDRIVER_OBJECT  DriverObject,
    _In_ PUNICODE_STRING RegistryPath
    )
{
    WDF_DRIVER_CONFIG config;
    WDFDRIVER         driver;
    NTSTATUS          status;

    HELLODRV_LOG_INFO("DriverEntry; RegistryPath=%wZ\n", RegistryPath);

    //
    // Software-only, non-PnP driver.
    // - No EvtDeviceAdd because we don't claim any hardware.
    // - We set EvtDriverUnload so we can unregister our callback cleanly
    //   when the service is stopped (`sc stop HelloDrv`).
    //
    WDF_DRIVER_CONFIG_INIT(&config, WDF_NO_EVENT_CALLBACK);
    config.DriverInitFlags = WdfDriverInitNonPnpDriver;
    config.EvtDriverUnload = HelloDrvEvtDriverUnload;

    status = WdfDriverCreate(
        DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        &driver);

    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("WdfDriverCreate failed: 0x%08X\n", status);
        return status;
    }

    //
    // Register the process create/exit callback. The "Ex" form gives us the
    // PS_CREATE_NOTIFY_INFO struct on creates (image name, command line,
    // parent PID, the option to deny the create with a NTSTATUS).
    //
    // NOTE: this kernel API requires the driver image to be linked with
    // /INTEGRITYCHECK. The WDK KMDF templates set this automatically.
    //
    status = PsSetCreateProcessNotifyRoutineEx(HelloDrvProcessNotify, FALSE);

    if (!NT_SUCCESS(status)) {
        HELLODRV_LOG_ERROR("PsSetCreateProcessNotifyRoutineEx(register) "
                           "failed: 0x%08X\n", status);
        // KMDF will clean up the WDFDRIVER object automatically when we
        // return a failure status from DriverEntry.
        return status;
    }

    HELLODRV_LOG_INFO("Registered process notify callback. Loaded.\n");
    return STATUS_SUCCESS;
}

//
// EvtDriverUnload: called when the service is stopped. We MUST unregister
// the process-notify callback here, otherwise the kernel will continue to
// invoke it after our image has been unloaded -> bugcheck.
//
VOID
HelloDrvEvtDriverUnload(
    _In_ WDFDRIVER Driver
    )
{
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    status = PsSetCreateProcessNotifyRoutineEx(HelloDrvProcessNotify, TRUE);

    if (!NT_SUCCESS(status)) {
        // Should never happen if DriverEntry succeeded; log and continue
        // unloading regardless. The OS guarantees no new invocations once
        // PsSetCreateProcessNotifyRoutineEx returns.
        HELLODRV_LOG_ERROR("PsSetCreateProcessNotifyRoutineEx(unregister) "
                           "failed: 0x%08X\n", status);
    }

    HELLODRV_LOG_INFO("Unloaded.\n");
}

//
// HelloDrvProcessNotify: invoked at IRQL == PASSIVE_LEVEL by the kernel
// every time a process is created or exits.
//
//   CreateInfo != NULL  -> process is being created; ProcessId is the new
//                          PID, CreateInfo->ParentProcessId is the parent.
//                          Setting CreateInfo->CreationStatus to a failure
//                          NTSTATUS would *block* the create (we don't).
//   CreateInfo == NULL  -> process is exiting; ProcessId is the dying PID.
//
// Documented constraints:
//   * Runs at IRQL <= PASSIVE_LEVEL.
//   * Must not block for long; this is in the create path of every process
//     on the box.
//
static VOID
HelloDrvProcessNotify(
    _Inout_     PEPROCESS              Process,
    _In_        HANDLE                 ProcessId,
    _Inout_opt_ PPS_CREATE_NOTIFY_INFO CreateInfo
    )
{
    UNREFERENCED_PARAMETER(Process);

    if (CreateInfo != NULL) {
        HELLODRV_LOG_INFO(
            "Process CREATE: PID=%llu, PPID=%llu, Image=\"%wZ\", "
            "CmdLine=\"%wZ\"\n",
            (ULONGLONG)(ULONG_PTR)ProcessId,
            (ULONGLONG)(ULONG_PTR)CreateInfo->ParentProcessId,
            CreateInfo->ImageFileName,
            CreateInfo->CommandLine);
    } else {
        HELLODRV_LOG_INFO(
            "Process EXIT:   PID=%llu\n",
            (ULONGLONG)(ULONG_PTR)ProcessId);
    }
}
