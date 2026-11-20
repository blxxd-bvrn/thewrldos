#!/bin/sh
# Builds the PE test vectors (x86 and x64) into rust/testdata/pe/. Needs MinGW-w64.
set -e
cd "$(dirname "$0")"
OUT=../../rust/testdata/pe
mkdir -p $OUT
for a in i686 x86_64; do
  cc=$a-w64-mingw32-gcc; wr=$a-w64-mingw32-windres; dt=$a-w64-mingw32-dlltool
  $wr testdll.rc -O coff -o /tmp/pe_res_$a.o
  $cc -O1 -s -shared -o $OUT/testdll_$a.dll testdll.c testdll.def /tmp/pe_res_$a.o -Wl,--dynamicbase
  $dt -d testdll_ord.def -l /tmp/pe_ord_$a.a
  $cc -O1 -c testexe.c -o /tmp/pe_exe_$a.o
  # the same program at two bases, both with relocations
  $cc -s -o $OUT/testexe_$a.exe /tmp/pe_exe_$a.o /tmp/pe_ord_$a.a -Wl,--dynamicbase -Wl,--image-base=0x10000000
  $cc -s -o $OUT/testexe_${a}_b.exe /tmp/pe_exe_$a.o /tmp/pe_ord_$a.a -Wl,--dynamicbase -Wl,--image-base=0x20000000
  rm -f /tmp/pe_res_$a.o /tmp/pe_ord_$a.a /tmp/pe_exe_$a.o
  # what objdump says, for the tests to compare with
  $a-w64-mingw32-objdump -p $OUT/testdll_$a.dll | python3 expect.py /dev/stdin > $OUT/testdll_$a.expect
  $a-w64-mingw32-objdump -p $OUT/testexe_$a.exe | python3 expect.py /dev/stdin > $OUT/testexe_$a.expect
done
ls -la $OUT
