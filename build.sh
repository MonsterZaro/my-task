#!/bin/bash
set -e
x86_64-w64-mingw32-g++ -static -o process_monitor.exe src/process_monitor.cpp -lpsapi
echo "Build successful: process_monitor.exe"
