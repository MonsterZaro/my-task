# my-task — Windows Process Monitor

A small Windows command-line utility for inspecting running processes and
reading their memory.

The program is written in C++ using only the Windows API (`<windows.h>`,
`<tlhelp32.h>`) and the C++ standard library, and ships as a single
statically-linked `.exe` with no external DLL dependencies beyond the standard
Windows system DLLs (`KERNEL32.dll`, `msvcrt.dll`).

## Features

- `--list` — enumerate every running process via
  `CreateToolhelp32Snapshot` / `Process32First` / `Process32Next` and print
  `PID`, parent `PID`, and the executable name.
- `--read <PID> <ADDRESS> <SIZE>` — open a target process with
  `OpenProcess(PROCESS_VM_READ | PROCESS_QUERY_INFORMATION)` and
  `ReadProcessMemory` `<SIZE>` bytes starting at `<ADDRESS>`. Output is a
  classic hex+ASCII dump.
- Friendly error messages for common failure modes (invalid PID, access
  denied / privilege errors, partial reads, etc.).

## Repository layout

```
.
├── build.sh                   # Cross-compile script (Linux + MinGW)
├── process_monitor.exe        # Pre-built static Windows x86_64 binary
├── src/
│   └── process_monitor.cpp    # Source
└── README.md
```

## Build

### Cross-compile from Linux with MinGW (recommended for CI / dev boxes)

Install the MinGW-w64 cross-compiler, then run the build script:

```bash
sudo apt-get update
sudo apt-get install -y g++-mingw-w64-x86-64

chmod +x build.sh
./build.sh
```

`build.sh` runs:

```bash
x86_64-w64-mingw32-g++ -static -o process_monitor.exe src/process_monitor.cpp -lpsapi
```

The `-static` flag produces a self-contained `.exe` that does not require
`libstdc++-6.dll`, `libgcc_s_seh-1.dll`, or `libwinpthread-1.dll` to be
shipped alongside it.

### Native build on Windows with MinGW

```cmd
g++ -static -o process_monitor.exe src\process_monitor.cpp -lpsapi
```

### Native build on Windows with MSVC (Developer Command Prompt)

```cmd
cl /EHsc /Fe:process_monitor.exe src\process_monitor.cpp Psapi.lib
```

## Usage

> The `.exe` requires **Windows** to run. Reading memory from another
> process generally requires **Administrator privileges**; right-click the
> terminal and choose **Run as administrator** before invoking `--read`,
> otherwise `OpenProcess` will fail with `ERROR_ACCESS_DENIED`.

### List all running processes

```cmd
process_monitor.exe --list
```

Example output:

```
PID     PPID    NAME
4       0       System
1234    4       svchost.exe
4321    1234    notepad.exe
...
```

### Read memory from a process

```
process_monitor.exe --read <PID> <ADDRESS> <SIZE>
```

- `<PID>` — decimal process ID (as shown by `--list` or Task Manager).
- `<ADDRESS>` — start address. Decimal (e.g. `4194304`) or hex with a `0x`
  prefix (e.g. `0x7ff600401000`).
- `<SIZE>` — number of bytes to read (decimal).

Example:

```cmd
process_monitor.exe --read 4321 0x7ff600401000 64
```

Example output:

```
Read 64 byte(s) from PID 4321 at 0x7ff600401000:
0x00007ff600401000  4d 5a 90 00 03 00 00 00 04 00 00 00 ff ff 00 00  |MZ..............|
0x00007ff600401010  b8 00 00 00 00 00 00 00 40 00 00 00 00 00 00 00  |........@.......|
...
```

## Notes / limitations

- The pre-built `process_monitor.exe` in this repository is x86_64 Windows
  only.
- The utility is intended for local debugging and learning; it does not
  attempt to bypass Windows protected-process or anti-debug protections.
  Reads from processes such as `csrss.exe`, `lsass.exe`, or processes
  running at higher integrity levels will fail with access-denied errors
  even when you are an Administrator.
- `--read` performs a single contiguous `ReadProcessMemory` call. If the
  range spans an unmapped page, the call fails and any partial bytes that
  were copied are still printed.
