#!/bin/sh
# Builds the API test program for both editions (needs MinGW-w64):
# apitest32.exe and apitest64.exe next to this script.
set -e
cd "$(dirname "$0")"
L="-lole32 -loleaut32 -lshlwapi -lshell32 -lversion -lwinmm -ladvapi32 -lnormaliz -lwinspool -luserenv"
i686-w64-mingw32-windres apitest.rc -O coff -o apitest_res32.o
i686-w64-mingw32-gcc -O1 -s -o apitest32.exe apitest.c apitest_res32.o $L
x86_64-w64-mingw32-windres apitest.rc -O coff -o apitest_res64.o
x86_64-w64-mingw32-gcc -O1 -s -o apitest64.exe apitest.c apitest_res64.o $L
rm -f apitest_res32.o apitest_res64.o
