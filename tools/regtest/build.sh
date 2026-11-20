#!/bin/sh
# Builds the registry test program for both editions (needs MinGW-w64):
# regtest32.exe and regtest64.exe next to this script.
set -e
cd "$(dirname "$0")"
i686-w64-mingw32-gcc -O1 -s -o regtest32.exe regtest.c -lshlwapi -ladvapi32
x86_64-w64-mingw32-gcc -O1 -s -o regtest64.exe regtest.c -lshlwapi -ladvapi32
