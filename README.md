# process_monitor

A small Windows command-line utility, written in C++, that uses the Win32
Toolhelp and Process APIs to:

- **List running processes** (PID, parent PID, thread count, image name).
- **Read a region of memory** from a target process and print a hex dump.

It is intended for educational use, debugging, and inspecting processes that
the operator owns or has explicit authorization to analyze.

## Features

- Process enumeration via `CreateToolhelp32Snapshot` / `Process32First` /
  `Process32Next` (`<tlhelp32.h>`).
- Memory reading via `OpenProcess` (with
  `PROCESS_VM_READ | PROCESS_QUERY_INFORMATION`) and `ReadProcessMemory`.
- Hex + ASCII dump output, 16 bytes per line.
- Friendly error reporting for the common failure cases:
  - access denied (insufficient privileges),
  - invalid PID (process doesn't exist),
  - bad address / unreadable memory region,
  - partial reads.
- Statically linked Windows binary — no external DLLs required beyond the
  ones that ship with Windows itself (`KERNEL32.dll`, `msvcrt.dll`).

## Repository layout

```
.
├── README.md
├── build.sh                 # Linux cross-compile script (MinGW-w64)
├── process_monitor.exe      # Pre-built x86_64 Windows binary
└── src/
    └── process_monitor.cpp  # C++ source
```

## Building

### Cross-compiling from Linux (MinGW-w64)

Install the MinGW cross-compiler, then run the bundled build script:

```bash
sudo apt-get update
sudo apt-get install -y g++-mingw-w64-x86-64
./build.sh
```

`build.sh` invokes:

```bash
x86_64-w64-mingw32-g++ -static -o process_monitor.exe src/process_monitor.cpp -lpsapi
```

The `-static` flag links the C/C++ runtime statically so the resulting
`process_monitor.exe` runs on a stock Windows install without any extra
runtime DLLs.

### Building natively on Windows

With MSYS2 / MinGW-w64 installed:

```bash
g++ -static -o process_monitor.exe src/process_monitor.cpp -lpsapi
```

With the MSVC toolchain (Developer Command Prompt):

```bat
cl /EHsc /Fe:process_monitor.exe src\process_monitor.cpp psapi.lib
```

## Usage

```text
process_monitor.exe --list
process_monitor.exe --read <PID> <HEX_ADDRESS> <SIZE>
process_monitor.exe --help
```

### List processes

```bat
process_monitor.exe --list
```

Sample output:

```
PID     PPID    THREADS NAME
------------------------------------------------------------
4       0       210     System
1234    1100    18      explorer.exe
5678    1234    7       notepad.exe
...
```

### Read memory

`HEX_ADDRESS` is parsed as hexadecimal (with or without a `0x` prefix).
`SIZE` is the number of bytes to read (decimal or hex). The maximum read
size is 16 MiB.

```bat
process_monitor.exe --read 5678 0x7FF6A1230000 64
```

Sample output:

```
Read 64 bytes from PID 5678 at 0x7ff6a1230000:
00007ff6a1230000  4d 5a 90 00 03 00 00 00  04 00 00 00 ff ff 00 00  |MZ..............|
00007ff6a1230010  b8 00 00 00 00 00 00 00  40 00 00 00 00 00 00 00  |........@.......|
00007ff6a1230020  00 00 00 00 00 00 00 00  00 00 00 00 00 00 00 00  |................|
00007ff6a1230030  00 00 00 00 00 00 00 00  00 00 00 00 e8 00 00 00  |................|
```

## Privileges

On Windows, opening another process with `PROCESS_VM_READ` is restricted by
the OS security model. In practice that means:

- **You usually need to run the command prompt as Administrator** to read
  memory of processes that aren't owned by your user.
- Even as Administrator, certain protected processes (e.g. `lsass.exe` with
  PPL, antivirus services, system processes) will refuse `OpenProcess`
  with `ERROR_ACCESS_DENIED`. This is by design.
- Reading processes you launched yourself (e.g. a `notepad.exe` you
  started) generally works without elevation.

Only use this tool against processes you own or have explicit permission
to inspect.

## Exit codes

| Code | Meaning                                              |
|------|------------------------------------------------------|
| 0    | Success                                              |
| 1    | Bad usage / unknown command / snapshot failure       |
| 2    | Argument parse error (bad PID, address, or size)     |
| 3    | `OpenProcess` failed (access denied, invalid PID)    |
| 4    | `ReadProcessMemory` failed (bad address, etc.)       |
