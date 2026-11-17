#!/usr/bin/env python3
"""Optional Windows-host SM3 validation; never linked into WRLD.

Run shadergputest with an output directory, then pass that directory here.
Disassembly alone accepts stage-illegal opcodes/read ports. Reassembly with
validation enabled checks those SM3 rules using Microsoft's host compiler.
"""
import argparse
import ctypes
import os
from pathlib import Path


def blob_data(blob):
    if not blob:
        return b""
    vt = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(vt[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(vt[4])(blob)
    data = ctypes.string_at(pointer, size).rstrip(b"\0")
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(vt[2])(blob)
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path, help="shadergputest's .sm3 export directory")
    args = parser.parse_args()
    if os.name != "nt":
        parser.error("this optional host check needs Windows and d3dcompiler_47.dll")
    paths = sorted(args.directory.glob("*.sm3"))
    if not paths:
        parser.error("no exported .sm3 programs found")
    dll = ctypes.WinDLL("d3dcompiler_47.dll")
    disassemble = dll.D3DDisassemble
    disassemble.restype = ctypes.c_long
    disassemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_uint,
                           ctypes.c_char_p, ctypes.POINTER(ctypes.c_void_p)]
    assemble = dll.D3DAssemble
    assemble.restype = ctypes.c_long
    assemble.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p,
                        ctypes.c_void_p, ctypes.c_void_p, ctypes.c_uint,
                        ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    failures = 0
    for path in paths:
        raw = path.read_bytes()
        text = ctypes.c_void_p()
        hr = disassemble(raw, len(raw), 0, None, ctypes.byref(text))
        if hr < 0:
            print(path.name, "disassembly failed", hex(hr & 0xFFFFFFFF))
            blob_data(text)
            failures += 1
            continue
        assembly = blob_data(text)
        path.with_suffix(".asm").write_bytes(assembly)
        program, errors = ctypes.c_void_p(), ctypes.c_void_p()
        hr = assemble(assembly, len(assembly), path.name.encode("utf-8"), None, None,
                      0, ctypes.byref(program), ctypes.byref(errors))
        message = blob_data(errors).decode(errors="replace")
        blob_data(program)
        if hr < 0:
            failures += 1
            print(path.name, "validation failed", hex(hr & 0xFFFFFFFF))
        if message:
            print(message)
    print(f"Microsoft SM3 assembler validation: {len(paths)} programs, {failures} failures")
    return bool(failures)


if __name__ == "__main__":
    raise SystemExit(main())
