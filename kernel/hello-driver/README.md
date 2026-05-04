# HelloDrv — a minimal KMDF "Hello, kernel" sample

A tiny, non-PnP KMDF kernel driver that registers a process-create/exit
callback and prints one `DbgPrintEx` line per event. It is intended as a
ground-truth starting point for learning Windows kernel development with
the WDK — small enough to read in one sitting, real enough to be useful.

## What it does

- Calls [`PsSetCreateProcessNotifyRoutineEx`](https://learn.microsoft.com/windows-hardware/drivers/ddi/ntddk/nf-ntddk-pssetcreateprocessnotifyroutineex)
  in `DriverEntry` to register a callback.
- On every **process create**, logs PID, PPID, image path, and command line.
- On every **process exit**, logs the PID.
- On **service stop / driver unload**, unregisters the callback (so the
  kernel doesn't call into a freed image and bugcheck the box) and logs an
  unload message.

It is deliberately:

- Non-PnP. No `IRP_MJ_*` dispatch, no device object, no symbolic link, no
  control codes. Use `sc create` to install, `sc start/stop` to load/unload.
- Single-file (`Driver.c`). Everything in ~200 lines including comments.
- Safe to load and unload repeatedly during development.

## Layout

```
hello-driver/
├── README.md              # This file.
└── HelloDrv/
    ├── Driver.c           # The driver source.
    └── HelloDrv.inx       # Optional INF template (for pnputil install).
```

## Prerequisites — the Windows DEV host

Install on a normal Windows 10/11 development workstation (NOT on the
target VM):

1. **Visual Studio 2022** (Community is fine):
   - During install, select **Desktop development with C++**.
   - In *Individual components*, select the latest **Windows 11 SDK**
     (`10.0.26100.*` at the time of writing).
   - Also select **MSVC v143 - VS 2022 C++ x64/x86 build tools** and
     **MSVC v143 Spectre-mitigated libs** (the WDK requires the
     Spectre-mitigated libs even for samples).
2. **Windows Driver Kit (WDK)** matching the SDK version — download from
   <https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk>.
   At the end of the WDK installer, **leave the "Install Visual Studio
   extension" checkbox ticked** so VS picks up the *Kernel Mode Driver*
   project templates.
3. (Optional, very useful) **DebugView** from
   <https://learn.microsoft.com/sysinternals/downloads/debugview>. We use
   it to watch `DbgPrintEx` output live on the test VM.
4. (Optional) **WinDbg** from
   <https://learn.microsoft.com/windows-hardware/drivers/debugger/>. The
   newer "WinDbg Preview" from the Microsoft Store is the most pleasant
   way to attach a kernel debugger over the network to your VM.

To verify the install, open VS → *File → New → Project*, search for
**“Kernel Mode Driver, Empty (KMDF)”**. If that template is present, the
WDK is wired up correctly.

## Prerequisites — the Windows TEST VM

Use a **Hyper-V Generation 2** Windows 10 or 11 VM with **checkpoints
enabled**. A bug in the driver = BSOD; checkpoints make recovery a
30-second affair.

> **Take a checkpoint NOW**, before you change any of the security
> settings below. They are easy to revert from a checkpoint, painful to
> revert by hand.

In an *Administrator* PowerShell **inside the VM**, run:

```powershell
# 1. Turn off Secure Boot in the VM firmware (Hyper-V Manager → VM →
#    Settings → Security → uncheck "Enable Secure Boot"). Do this with
#    the VM shut down, then start the VM again.

# 2. Disable HVCI / Memory Integrity. Settings → Privacy & security →
#    Windows Security → Device security → Core isolation details →
#    Memory integrity = OFF. Reboot when prompted.
#    On Server / LTSC builds you may need:
reg add "HKLM\SYSTEM\CurrentControlSet\Control\DeviceGuard\Scenarios\HypervisorEnforcedCodeIntegrity" /v Enabled /t REG_DWORD /d 0 /f

# 3. Enable test-signing. After reboot, the desktop will have a "Test
#    Mode" watermark in the bottom-right corner -- that is how you know
#    test-signed drivers will load.
bcdedit /set testsigning on

# 4. (Optional, for live kernel debugging) enable a kernel debugger over
#    the network. Replace HOSTIP with the dev host's IP.
bcdedit /debug on
bcdedit /dbgsettings net hostip:HOSTIP port:50000 key:1.2.3.4

shutdown /r /t 0
```

After reboot, confirm:

- `bcdedit /enum {current}` shows `testsigning Yes`.
- The "Test Mode" watermark is visible on the desktop.
- `Get-CimInstance Win32_DeviceGuard | Select-Object SecurityServicesRunning`
  does **not** include the value `2` (HVCI).

## Building

### Option A — drop into the WDK template (recommended for first-timers)

1. In VS 2022 on the dev host: *File → New → Project → "Kernel Mode
   Driver, Empty (KMDF)"*. Name it `HelloDrv`.
2. Delete the auto-generated `Driver.c` if VS created one.
3. **Add → Existing item…** and point at the `HelloDrv\Driver.c` from this
   repo. (And `HelloDrv.inx` if you want the INF install path; otherwise
   skip it — `sc create` works fine without an INF.)
4. Open the project's properties:
   - *Configuration: All configurations, Platform: x64*.
   - *Driver Settings → General → Target OS Version*: Windows 10 or
     later.
   - *Driver Settings → General → Target Platform*: Desktop.
   - *Driver Signing → General → Sign Mode*: **Test Sign**.
   - *Driver Signing → Test Certificate*: leave as default; VS auto-
     generates a `WDKTestCert <user>,<sha1>` cert and installs it into
     `Cert:\CurrentUser\My`.
5. *Build → Build Solution*. Output:

   ```
   HelloDrv\x64\Debug\HelloDrv\HelloDrv.sys
   HelloDrv\x64\Debug\HelloDrv\HelloDrv.cat        (only if you used the INF)
   HelloDrv\x64\Debug\HelloDrv\HelloDrv.inf        (only if you used the INF)
   HelloDrv\x64\Debug\HelloDrv\WDKTestCert*.cer    (the test cert)
   ```

### Option B — command line with `MSBuild`

From a *Developer Command Prompt for VS 2022*:

```cmd
msbuild HelloDrv.sln /p:Configuration=Debug /p:Platform=x64
```

You'll still need the project to exist — Option A creates it.

## Installing the test cert on the VM

Copy `WDKTestCert*.cer` from the dev host to the VM, then in an
**Administrator** PowerShell **on the VM**:

```powershell
# Install into the two stores the OS checks at driver load.
certutil -addstore -f Root        WDKTestCert.cer
certutil -addstore -f TrustedPublisher WDKTestCert.cer
```

You only need to do this once per VM; subsequent rebuilds reuse the same
cert.

## Installing the driver on the VM

Copy `HelloDrv.sys` to the VM (e.g. `C:\Drivers\HelloDrv.sys`), then in an
*Administrator* command prompt:

```cmd
sc create HelloDrv type= kernel binPath= C:\Drivers\HelloDrv.sys start= demand
sc start  HelloDrv
```

The expected output is `STATE: 4 RUNNING`. If `sc start` fails, the most
common error codes are:

| Error                                            | Meaning                                                    | Fix                                                                                |
| ------------------------------------------------ | ---------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| `577` (`ERROR_DRIVER_BLOCKED`)                   | HVCI / Memory Integrity is still on, or Secure Boot is on. | Disable both as above and reboot.                                                  |
| `1275` (`ERROR_DRIVER_BLOCKED_LOAD`)             | Test cert isn't trusted on this VM.                        | `certutil -addstore Root` / `TrustedPublisher` with the `.cer` file.               |
| `1058` (`ERROR_SERVICE_DISABLED`)                | Service start type set wrong.                              | `sc config HelloDrv start= demand`.                                                |
| `5` (`ERROR_ACCESS_DENIED`)                      | Not running elevated.                                      | Run the prompt as Administrator.                                                   |

## Watching the output

### DebugView (simplest)

1. Run `DbgView.exe` on the VM **as Administrator**.
2. *Capture menu*: enable **Capture Kernel** and **Enable Verbose Kernel
   Output**. Optionally **Capture Win32** off to reduce noise.
3. Re-`sc start HelloDrv` and start a few processes (open Notepad, quit
   it). You should see lines like:

   ```
   [HelloDrv] DriverEntry; RegistryPath=\REGISTRY\MACHINE\System\CurrentControlSet\Services\HelloDrv
   [HelloDrv] Registered process notify callback. Loaded.
   [HelloDrv] Process CREATE: PID=4321, PPID=1234, Image="\Device\HarddiskVolume3\Windows\System32\notepad.exe", CmdLine=""notepad.exe""
   [HelloDrv] Process EXIT:   PID=4321
   ```

   If you see the `DriverEntry` line but no `Process CREATE` lines, your
   process notify callback returned a non-success status — check
   DebugView for the "PsSetCreateProcessNotifyRoutineEx ... failed" error
   message.

### WinDbg (kernel debugger attached over the network)

In a kernel-debug session attached to the VM:

```
kd> ed nt!Kd_DEFAULT_Mask 0xFFFFFFFF
kd> ed nt!Kd_IHVDRIVER_Mask 0xFFFFFFFF
```

Then `g` to let the VM run; `[HelloDrv] ...` lines will appear in the
debugger output.

## Stopping / removing

```cmd
sc stop   HelloDrv
sc delete HelloDrv
```

Confirm in DebugView you see `[HelloDrv] Unloaded.` between the `stop`
and `delete`.

## Common pitfalls

- **BSOD `DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS`** on
  `sc stop`: usually means a callback was not unregistered. Make sure
  `EvtDriverUnload` calls `PsSetCreateProcessNotifyRoutineEx(..., TRUE)`
  before returning.
- **`Process CREATE` lines have garbage in `Image` / `CmdLine`**: those
  fields can be `NULL` if `PROCESS_CREATE_FLAGS_FILE_SHORT_NAME` is set
  for that process or for kernel-created processes. The sample's
  `%wZ` formatter handles `NULL` `UNICODE_STRING` pointers safely on
  Windows 10+, but if you hit it on older builds, gate with
  `if (CreateInfo->ImageFileName) ...`.
- **`PsSetCreateProcessNotifyRoutineEx` fails with `0xC0000022`
  `STATUS_ACCESS_DENIED`**: the driver image isn't linked with
  `/INTEGRITYCHECK`. WDK templates set this; if you're rolling your own
  project file, add `<IntegrityCheck>true</IntegrityCheck>` under
  `<Link>`.
- **Driver loads but no callback fires**: HVCI/Memory Integrity quietly
  killed your callback registration on Windows 11. Re-check it's off.

## Where to go next

Once this driver loads and you see live process events:

- Add a control device (`WdfDeviceCreate` + a symbolic link) and an IOCTL
  so a user-mode helper can subscribe to events instead of dumping them
  to `DbgPrint`.
- Look at the official samples in
  <https://github.com/microsoft/Windows-driver-samples>, especially
  `general/echo` (KMDF basics) and `filesys/miniFilter` (file system
  callbacks).
- Read [*Windows Kernel Programming, 2nd ed.* by Pavel Yosifovich] —
  the canonical practical book on KMDF.
- Read the WDK [Driver Verifier docs](https://learn.microsoft.com/windows-hardware/drivers/devtest/driver-verifier)
  and turn it on for `HelloDrv` (`verifier /standard /driver HelloDrv.sys`)
  the first time you change anything non-trivial. It catches a huge
  fraction of kernel bugs at the moment they happen instead of two
  reboots later.

## License

Public domain / MIT-style: do whatever you want with this sample, no
warranty. Don't ship it as a product without removing the `DbgPrintEx`
spam.
