# HelloDrv — KMDF process / image / thread monitor

A small but feature-complete Windows kernel research project:

- **`HelloDrv.sys`** — a non-PnP KMDF kernel driver that subscribes to three
  documented OS notification APIs and exposes the events to user mode via
  an IOCTL interface.
- **`HelloDrvMonitor.exe`** — a user-mode subscriber that opens
  `\\.\HelloDrv`, blocks for events, and prints them in a human-readable
  table or as one JSON object per line for piping into a log analyzer.

It is intentionally observational. The driver does **not** read another
process's memory, hide processes, hook anti-cheat, or do anything else
that interferes with security software — that is out of scope for this
project, and code for that will not be added.

## What the driver hooks into

| Notification | API | What we capture |
| --- | --- | --- |
| Process create | `PsSetCreateProcessNotifyRoutineEx` | PID, PPID, image path, command line |
| Process exit  | `PsSetCreateProcessNotifyRoutineEx` | PID |
| Image load    | `PsSetLoadImageNotifyRoutine`       | PID, image path, kernel-image flag |
| Thread create | `PsSetCreateThreadNotifyRoutine`    | PID, TID |
| Thread exit   | `PsSetCreateThreadNotifyRoutine`    | PID, TID |

The driver never sets `CreateInfo->CreationStatus`, so it can never block a
process from starting.

## Architecture

```
  +----------------------------+        +-------------------------------+
  |  Kernel: HelloDrv.sys      |        |  User: HelloDrvMonitor.exe    |
  |                            |        |                               |
  |  PsSet*NotifyRoutine* -+   |        |   CreateFile("\\\\.\\HelloDrv") |
  |                        |   |        |        |                      |
  |                        v   |        |        v                      |
  |   AllocEvent ----> DispatchEvent <---- IOCTL_HELLODRV_GET_NEXT_EVENT |
  |                        |   |        |        |                      |
  |     +------------------+---+        |        v                      |
  |     |                  |   |        |  PrintEvent / JSON / filter  |
  |     v                  v   |        |                               |
  |  pending IRP queue   ring buffer    |                               |
  |  (inverted call)     (512 max,      |                               |
  |                       bounded FIFO) |                               |
  +----------------------------+        +-------------------------------+
```

- **Inverted call.** When user-mode calls `IOCTL_HELLODRV_GET_NEXT_EVENT`
  and there is no event ready, the driver parks the IRP on a manual
  KMDF queue and returns. The next time a notify callback fires, the
  driver pulls the IRP back off the manual queue and completes it with
  the event payload. No polling, no shared memory, no kernel events
  exposed to user mode.

- **Bounded ring buffer.** When events arrive faster than user mode is
  draining them, they are queued on a non-paged-pool linked list capped
  at 512 entries. If the cap is hit, the oldest event is dropped and
  `Stats.EventsDroppedDueToBackpressure` is incremented. There is no
  unbounded growth and no risk of exhausting non-paged pool.

- **Stats.** `IOCTL_HELLODRV_GET_STATS` returns a `HELLODRV_STATS` struct
  with per-event-type counters and the dropped/delivered/pending tallies.
  `IOCTL_HELLODRV_RESET_STATS` zeroes them.

## Repository layout

```
kernel/hello-driver/
├── README.md                            # You are here.
├── HelloDrv.sln                         # VS solution: open this in VS 2022+.
├── HelloDrv/                            # Kernel driver source (build on Windows + WDK).
│   ├── HelloDrv.vcxproj                 # KMDF driver project (x64 Debug + Release).
│   ├── HelloDrv.vcxproj.Filters         # Solution Explorer source organization.
│   ├── Driver.c                         # DriverEntry, IOCTL handler, notify callbacks.
│   ├── Driver.h                         # Internal kernel-only declarations.
│   ├── Public.h                         # SHARED header: IOCTLs + event/stats structs.
│   └── HelloDrv.inx                     # Optional INF for `pnputil /add-driver`.
└── HelloDrvMonitor/                     # User-mode subscriber (build on Windows or Linux+MinGW).
    ├── HelloDrvMonitor.vcxproj          # User-mode console-app project.
    ├── HelloDrvMonitor.vcxproj.Filters  # Solution Explorer source organization.
    ├── Monitor.c                        # Open device, blocking IOCTL loop, pretty-print.
    ├── build.sh                         # MinGW cross-compile script (Linux).
    └── HelloDrvMonitor.exe              # Pre-built static x86_64 binary (MinGW).
```

`Public.h` is included from **both** the driver and the user-mode tool.
That is the only header that crosses the kernel/user-mode boundary.

## Prerequisites

### Dev host (where you compile)

- **Visual Studio 2022** + Desktop C++ workload + Spectre-mitigated libs.
- **Windows 11 SDK** matching the WDK (`10.0.26100.*` at the time of writing).
- **WDK** + WDK Visual Studio extension —
  <https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk>.
- (Optional) MinGW-w64 (`g++-mingw-w64-x86-64` on Debian/Ubuntu) if you
  want to cross-build the user-mode tool from Linux.
- (Optional) **DebugView** and/or **WinDbg** for live log inspection.

To verify VS+WDK: *File → New → Project → "Kernel Mode Driver, Empty
(KMDF)"* should appear.

### Test VM (where you run)

A **Hyper-V Generation 2** Windows 10 / 11 VM with **checkpoints**. Take a
checkpoint *now* before you change anything below.

In an Administrator PowerShell on the VM:

```powershell
# Disable Secure Boot in the VM firmware (Hyper-V Manager → VM → Settings →
# Security → uncheck "Enable Secure Boot"), with the VM shut down.

# Disable HVCI / Memory Integrity:
#   Settings → Privacy & security → Windows Security → Device security →
#   Core isolation → Memory integrity = OFF. Reboot.

# Enable test signing:
bcdedit /set testsigning on

# (Optional) network kernel debugger to your dev host:
bcdedit /debug on
bcdedit /dbgsettings net hostip:HOSTIP port:50000 key:1.2.3.4

shutdown /r /t 0
```

After reboot:

- `bcdedit /enum {current}` shows `testsigning Yes`.
- The desktop has a "Test Mode" watermark.
- `Get-CimInstance Win32_DeviceGuard | select SecurityServicesRunning` does
  **not** include the value `2`.

## Building

### 1. Open the solution

```
kernel\hello-driver\HelloDrv.sln
```

The solution contains two projects:

| Project           | ConfigurationType | PlatformToolset                | Output                     |
| ----------------- | ----------------- | ------------------------------ | -------------------------- |
| `HelloDrv`        | `Driver`          | `WindowsKernelModeDriver10.0`  | `HelloDrv.sys` (+ `.inf`, `.cat`, `WDKTestCert*.cer`) |
| `HelloDrvMonitor` | `Application`     | `v143` (VS 2022 MSVC)          | `HelloDrvMonitor.exe`      |

Both projects ship with three configurations:

| Configuration            | Spectre mitigation | Optimization | Intended use                                                                 |
| ------------------------ | ------------------ | ------------ | ---------------------------------------------------------------------------- |
| `Debug \| x64`            | Spectre            | Disabled     | Day-to-day development. Symbols, no inlining.                                |
| `Release \| x64`          | Spectre            | MaxSpeed     | **Default. Use this for the `.sys` you ship to the test VM.**                |
| `Release-NoSpectre \| x64` | **off**            | MaxSpeed     | Escape hatch when the Spectre-mitigated libs for your toolset aren't on the host. Do not deploy artifacts from this config. |

For the deployable `.sys` you ship to the test VM, switch the configuration
dropdown in the VS toolbar to **Release | x64** before building.

If your VS install is a newer version than 17.x, the IDE will offer to retarget
the projects on first open — accept the defaults. The retarget binds the
projects to whatever toolset and Windows SDK / WDK your install has.

### 2. One-time prerequisites in the VS Installer

The solution will fail to build if these aren't present. Open the Visual
Studio Installer → *Modify* on your VS install:

- **Workloads** → *Desktop development with C++*.
- **Individual components**:
  - *MSVC v143 - VS 2022 C++ x64/x86 build tools (Latest)* (or your VS's equivalent).
  - **MSVC v143 — VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)**
    *(required by `<SpectreMitigation>Spectre</SpectreMitigation>` — without
    this you'll get "spectre-mitigated runtime library not found" at link time)*.
  - *Latest Windows 11 SDK*.
  - *C++ ATL for latest v143 build tools (x86 & x64)*  *(WDK transitively wants this)*.

Plus the WDK itself (separate installer):
<https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk>.
The WDK installer ends by offering to install the *WDK Visual Studio
extension* — say yes; without it VS won't see the `WindowsKernelModeDriver10.0`
toolset and the `HelloDrv` project will fail to load.

### 3. Build

In Visual Studio: select **Release | x64** in the toolbar → *Build → Build Solution*.

Output goes to `kernel\hello-driver\build\x64\Release\<ProjectName>\`:

```
build\x64\Release\HelloDrv\HelloDrv.sys
build\x64\Release\HelloDrv\HelloDrv.inf
build\x64\Release\HelloDrv\HelloDrv.cat
build\x64\Release\HelloDrv\WDKTestCert*.cer
build\x64\Release\HelloDrvMonitor\HelloDrvMonitor.exe
```

The `WDKTestCert*.cer` is auto-generated by the WDK signing step using the
identity you configure under *Project → Properties → Driver Signing →
Test Certificate*. The first build will create one for you; subsequent
builds re-use it.

#### `MSB8040: Spectre-mitigated libraries are required for this project`

The `Release | x64` and `Debug | x64` configurations both set
`<SpectreMitigation>Spectre</SpectreMitigation>`, so the build will fail
with `MSB8040` if the Spectre-mitigated runtime libs for your **toolset** —
not just your VS major version — aren't installed.

In VS 2022 (toolset `v143`) install:
*MSVC v143 — VS 2022 C++ x64/x86 Spectre-mitigated libs (Latest)*.

In VS Insiders / preview builds the toolset may be `v180` (or whatever the
current preview ships); the v143 libs are **not** interchangeable. Look in
the VS Installer's *Individual components* tab for the matching Spectre libs
component for *your* toolset.

If your VS install doesn't yet have a Spectre-libs component for its
toolset (e.g. very early Insiders previews), use the
**`Release-NoSpectre | x64`** configuration as a temporary unblock:

```
build\x64\Release-NoSpectre\HelloDrv\HelloDrv.sys
build\x64\Release-NoSpectre\HelloDrvMonitor\HelloDrvMonitor.exe
```

Artifacts under `build\x64\Release-NoSpectre\` are **not Spectre-mitigated**
and exist only to let you exercise the rest of the toolchain (compile, sign,
load on the VM). Do not ship them anywhere you care about Spectre v1/v2
mitigation; switch back to `Release | x64` for that as soon as the matching
Spectre libs are available.

The driver source defines `HELLODRV_NO_SPECTRE_BUILD=1` only when this
config is selected, so a future safety check (e.g. refusing to load on
production hosts) can key off it cleanly.

### Building the user-mode tool from Linux (alternative)

If you only need the `.exe` and not a full VS install, the user-mode tool
also cross-compiles from Linux with MinGW:

```bash
sudo apt-get install -y g++-mingw-w64-x86-64
cd kernel/hello-driver/HelloDrvMonitor
chmod +x build.sh
./build.sh
```

Output: a single statically-linked `HelloDrvMonitor.exe` importing only
`KERNEL32.dll` and `msvcrt.dll`. The kernel driver `.sys` itself still
has to be built on Windows with the WDK.

## Installing & running on the test VM

1. Copy `HelloDrv.sys`, `HelloDrvMonitor.exe`, and the `WDKTestCert*.cer`
   to the VM (e.g. via a Hyper-V file share or `scp`).
2. Trust the test cert on the VM:

   ```cmd
   certutil -addstore -f Root              WDKTestCert.cer
   certutil -addstore -f TrustedPublisher  WDKTestCert.cer
   ```

3. Install and start the kernel service:

   ```cmd
   sc create HelloDrv type= kernel binPath= C:\Drivers\HelloDrv.sys start= demand
   sc start  HelloDrv
   ```

   Expected: `STATE: 4 RUNNING`.

4. Run the user-mode subscriber **as Administrator**:

   ```cmd
   HelloDrvMonitor.exe
   ```

   Open Notepad, close it, and you should see something like:

   ```
   [HelloDrvMonitor] Subscribed. Press Ctrl+C to stop.
   [2026-05-04 13:47:02.413] PROCESS_CREATE pid=4321  ppid=1234  image="\Device\HarddiskVolume3\Windows\System32\notepad.exe" cmd=""notepad.exe""
   [2026-05-04 13:47:02.418] IMAGE_LOAD     pid=4321  kernel=false image="\Device\HarddiskVolume3\Windows\System32\notepad.exe"
   [2026-05-04 13:47:02.420] IMAGE_LOAD     pid=4321  kernel=false image="\Device\HarddiskVolume3\Windows\System32\ntdll.dll"
   [2026-05-04 13:47:02.422] IMAGE_LOAD     pid=4321  kernel=false image="\Device\HarddiskVolume3\Windows\System32\kernel32.dll"
   [2026-05-04 13:47:02.430] THREAD_CREATE  pid=4321  tid=8765
   ...
   [2026-05-04 13:47:09.107] THREAD_EXIT    pid=4321  tid=8765
   [2026-05-04 13:47:09.112] PROCESS_EXIT   pid=4321
   ```

5. Useful flags:

   ```cmd
   HelloDrvMonitor.exe --no-images --no-threads          REM Quietest view.
   HelloDrvMonitor.exe --pid 4321                        REM Filter to one PID.
   HelloDrvMonitor.exe --image notepad                   REM Substring match.
   HelloDrvMonitor.exe --json | python -m json.tool      REM JSON-Lines output.
   HelloDrvMonitor.exe --stats                           REM Live counters every 1s.
   HelloDrvMonitor.exe --reset-stats                     REM Zero counters and exit.
   ```

6. Stop / remove:

   ```cmd
   sc stop   HelloDrv
   sc delete HelloDrv
   ```

   You should see `[HelloDrv] Unloaded.` in DebugView between `stop` and
   `delete`.

## IOCTL interface

All IOCTLs are `METHOD_BUFFERED`. Definitions live in
`HelloDrv/Public.h`.

### `IOCTL_HELLODRV_GET_NEXT_EVENT`

- Input: none.
- Output: `HELLODRV_EVENT` (~3 KB, fixed size).
- Behavior: returns the next queued event immediately, or blocks the
  request until the next process / image / thread notification fires.
  `bytes_returned == sizeof(HELLODRV_EVENT)` on success.

### `IOCTL_HELLODRV_GET_STATS`

- Input: none.
- Output: `HELLODRV_STATS`.
- Behavior: returns a snapshot of the per-event-type counters.

### `IOCTL_HELLODRV_RESET_STATS`

- Input: none.
- Output: none.
- Behavior: zeroes all counters.

## Common failures

| Symptom                                                  | Likely cause                                              | Fix                                                                  |
| -------------------------------------------------------- | --------------------------------------------------------- | -------------------------------------------------------------------- |
| `sc start` returns `577 ERROR_DRIVER_BLOCKED`            | HVCI / Memory Integrity is on, or Secure Boot is on       | Disable both, reboot                                                 |
| `sc start` returns `1275 ERROR_DRIVER_BLOCKED_LOAD`      | Test cert isn't trusted on the VM                         | `certutil -addstore Root` + `TrustedPublisher` with `WDKTestCert.cer` |
| `DriverEntry` logs `STATUS_ACCESS_DENIED` from `PsSet…Ex` | Driver image not linked with `/INTEGRITYCHECK`            | Use the WDK template; or set `<IntegrityCheck>true</IntegrityCheck>` |
| Loads, but `Process CREATE` lines never appear           | HVCI quietly disabled the callback registration           | Confirm HVCI is off; reboot                                          |
| `CreateFile("\\\\.\\HelloDrv")` returns 5 `ACCESS_DENIED` | User-mode tool isn't running as Administrator             | Run elevated (the SDDL only allows SYSTEM and BUILTIN\\Administrators) |
| BSOD `DRIVER_UNLOADED_WITHOUT_CANCELLING_PENDING_OPERATIONS` on `sc stop` | Callbacks not unregistered before the image is freed | Don't modify `EvtDriverUnload` to skip the `PsRemove*` calls        |

## Driver Verifier

Whenever you change the driver, turn Driver Verifier on for it before
testing:

```cmd
verifier /standard /driver HelloDrv.sys
shutdown /r /t 0
```

After reboot, the Verifier flags will catch most kernel bugs at the moment
they happen, instead of a few reboots later. Turn it off with
`verifier /reset` once you're done.

## What's deliberately NOT here

- No `MmCopyVirtualMemory` / `NtReadVirtualMemory` / `KeStackAttachProcess`
  IOCTL. The driver does not read another process's address space.
- No `PsActiveProcessHead` traversal, no DKOM, no `EPROCESS` patching, no
  hiding of the driver, of its service, or of any process from any tool.
- No SSDT / IRP / inline / IAT hooks; no patching of any kernel callback;
  no interference with anti-cheat, anti-virus, or PatchGuard.

If your project needs any of those primitives, this is not the right
starting point and I will not extend it in that direction.

## Where to go next (legitimate)

- Add a `WdfFileObject` per-handle context so each opener gets its own
  per-handle event queue (currently all openers share one ring).
- Replace the inline-string event payload with a variable-length packed
  format so events for very long command lines aren't truncated.
- Add a Filter Manager mini-filter (`FltRegisterFilter`) for file system
  events. See the WDK `filesys/miniFilter` samples.
- Add ETW-style structured logging (TraceLogging) instead of `DbgPrintEx`.
- Read [*Windows Kernel Programming, 2nd ed.* by Pavel Yosifovich] and
  the WDK [Driver Verifier docs](https://learn.microsoft.com/windows-hardware/drivers/devtest/driver-verifier).

## License

Public domain / MIT-style: do whatever you want with this sample, no
warranty.
