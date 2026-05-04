#!/bin/bash
# Cross-compile the user-mode HelloDrvMonitor.exe from Linux with MinGW.
# The kernel driver itself must be built on Windows with the WDK; this
# script only builds the user-mode subscriber.
set -euo pipefail
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -Wall -Wextra -static -O2 \
    -o HelloDrvMonitor.exe Monitor.c
