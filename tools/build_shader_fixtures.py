#!/usr/bin/env python3
"""Regenerate shader test data using real Microsoft shader compilers on Windows.

D3DCompile from the system d3dcompiler_47.dll emits SM4/SM5 DXBC. Supply --dxc
with the host DXC executable to also regenerate SM6 DXIL. The resulting small
fixtures are checked in, so ordinary host tests need neither compiler nor DLL.
"""
import argparse
import ctypes
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "tools/shaders/graphics.hlsl"


def write_lf(path, text):
    with path.open("w", encoding="ascii", newline="\n") as out:
        out.write(text)


def normalize_dump(path):
    write_lf(path, "\n".join(line.rstrip() for line in path.read_text(encoding="ascii").splitlines()) + "\n")


def blob_bytes(blob):
    table = ctypes.cast(blob, ctypes.POINTER(ctypes.POINTER(ctypes.c_void_p))).contents
    pointer = ctypes.WINFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)(table[3])(blob)
    size = ctypes.WINFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)(table[4])(blob)
    data = ctypes.string_at(pointer, size)
    ctypes.WINFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)(table[2])(blob)
    return data


def fxc(source, entry, target):
    compile_shader = ctypes.WinDLL("d3dcompiler_47.dll").D3DCompile
    compile_shader.restype = ctypes.c_long
    compile_shader.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_char_p, ctypes.c_void_p,
                               ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint,
                               ctypes.c_uint, ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_void_p)]
    code, errors = ctypes.c_void_p(), ctypes.c_void_p()
    result = compile_shader(source, len(source), b"graphics.hlsl", None, None, entry.encode(), target.encode(),
                            (1 << 15) | (1 << 11), 0, ctypes.byref(code), ctypes.byref(errors))
    message = blob_bytes(errors).decode(errors="replace") if errors else ""
    if result < 0:
        raise RuntimeError(f"{entry}/{target}: {message}")
    return blob_bytes(code)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dxc", type=Path)
    ap.add_argument("--skip-dxbc", action="store_true")
    ap.add_argument("--out", type=Path, default=ROOT / "tools/shaders")
    args = ap.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    source = SOURCE.read_bytes()
    # FXC does not know wave operations, even when compiling another entry.
    legacy = source.split(b"// Deliberately unsupported", 1)[0]
    common = [("vertex", "vs"), ("color", "ps"), ("texture_level", "ps"), ("reciprocal", "ps"),
              ("packed_vertex", "vs"), ("packed_pixel", "ps"), ("sparse", "ps"), ("maths", "ps"),
              ("depth", "ps"), ("flat_color", "ps"), ("linear_color", "ps"), ("sample_implicit", "ps"),
              ("derivative", "ps"), ("cube_texture", "ps"), ("smooth_color", "ps"),
              ("clip_color", "ps"), ("uint_vertex", "vs")]
    if not args.skip_dxbc:
        for model in (4, 5):
            entries = list(common)
            if model == 5:
                entries += [("bits", "ps"), ("half_round", "ps"), ("lod_one", "ps")]
            for entry, stage in entries:
                target = f"{stage}_{model}_0"
                data = fxc(legacy, entry, target)
                path = args.out / f"{entry}-{target}.dxbc"
                path.write_bytes(data)
                print(path.name, len(data))
    if args.dxc:
        for entry, stage in common + [("bits", "ps"), ("half_round", "ps"), ("lod_one", "ps"), ("wave", "ps"),
                                      ("compute", "cs"), ("dynamic_loop", "ps")]:
            target = f"{stage}_6_0"
            path = args.out / f"{entry}-{target}.dxil"
            subprocess.run([str(args.dxc), "-T", target, "-E", entry, "-O3", "-Ges", "-Qstrip_debug",
                            "-Fo", str(path), "-Fc", str(path.with_suffix(".ll")), str(SOURCE)], check=True)
            normalize_dump(path.with_suffix(".ll"))
            print(path.name, path.stat().st_size)
        rejected = ROOT / "tools/shaders/rejected.hlsl"
        for entry in ("resource_space", "uav", "helper_call", "double_math", "newer_profile"):
            target = "ps_6_1" if entry == "newer_profile" else "ps_6_0"
            path = args.out / f"{entry}-{target}.dxil"
            subprocess.run([str(args.dxc), "-T", target, "-E", entry, "-O3", "-Ges", "-Qstrip_debug",
                            "-Fo", str(path), "-Fc", str(path.with_suffix(".ll")), str(rejected)], check=True)
            normalize_dump(path.with_suffix(".ll"))
            print(path.name, path.stat().st_size)
    cube = ROOT / "samples/d3d10cube.hlsl"
    for model in (5, 6):
        if (model == 5 and args.skip_dxbc) or (model == 6 and not args.dxc):
            continue
        for entry, stage in (("vertex", "vs"), ("pixel", "ps")):
            target = f"{stage}_{model}_0"
            path = args.out / f"cube-{entry}-{target}.{'dxil' if model == 6 else 'dxbc'}"
            if model == 5:
                path.write_bytes(fxc(cube.read_bytes(), entry, target))
            else:
                subprocess.run([str(args.dxc), "-T", target, "-E", entry, "-O3", "-Ges", "-Qstrip_debug",
                                "-Fo", str(path), "-Fc", str(path.with_suffix(".ll")), str(cube)], check=True)
                normalize_dump(path.with_suffix(".ll"))
            print(path.name, path.stat().st_size)
    cube_paths = [(f"g_{stage}{model}", args.out / f"cube-{entry}-{stage}_{model}_0.{'dxil' if model == 6 else 'dxbc'}")
                  for model in (5, 6) for entry, stage in (("vertex", "vs"), ("pixel", "ps"))]
    if all(path.exists() for _, path in cube_paths):
        lines = ["/* Generated by tools/build_shader_fixtures.py from samples/d3d10cube.hlsl. */",
                 "#ifndef D3D10CUBE_MODERN_SHADERS_H", "#define D3D10CUBE_MODERN_SHADERS_H"]
        for name, path in cube_paths:
            data = path.read_bytes()
            lines.append(f"static const unsigned char {name}[] = {{")
            lines += ["    " + ", ".join(f"0x{x:02x}" for x in data[i:i + 16]) + "," for i in range(0, len(data), 16)]
            lines.append("};")
        lines.append("#endif")
        write_lf(ROOT / "samples/d3d10cube_modern_shaders.h", "\n".join(lines) + "\n")
    probe_paths = [(f"g_reject_{name}", args.out / path) for name, path in (
        ("wave", "wave-ps_6_0.dxil"), ("compute", "compute-cs_6_0.dxil"),
        ("lod", "lod_one-ps_6_0.dxil"), ("space", "resource_space-ps_6_0.dxil"),
        ("uav", "uav-ps_6_0.dxil"), ("profile", "newer_profile-ps_6_1.dxil"))]
    if all(path.exists() for _, path in probe_paths):
        lines = ["/* Generated by tools/build_shader_fixtures.py; genuine DXC rejection fixtures. */",
                 "#ifndef SHADERPROBE_SHADERS_H", "#define SHADERPROBE_SHADERS_H"]
        for name, path in probe_paths:
            data = path.read_bytes()
            lines.append(f"static const unsigned char {name}[] = {{")
            lines += ["    " + ", ".join(f"0x{x:02x}" for x in data[i:i + 16]) + "," for i in range(0, len(data), 16)]
            lines.append("};")
        lines.append("#endif")
        write_lf(ROOT / "samples/shaderprobe_shaders.h", "\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
