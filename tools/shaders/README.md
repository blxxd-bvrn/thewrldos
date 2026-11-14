# Native shader tests and staged compatibility

These are **owned HLSL sources and genuine Microsoft-compiler outputs**, not a
private bytecode dialect. The engine stays native C; the compilers are host
fixture-generation tools, never OS runtime dependencies.

## Tested scope

| Frontend | Accepted stage/profile subset |
| --- | --- |
| DXBC | Vertex/pixel SM4.0/4.1 and SM5.0 token programs in DXBC containers |
| DXIL | Vertex/pixel SM6.0, DXIL 1.0, LLVM bitcode version 1; one void entry function |

`dxbc.c` and `dxil.c` lower to `shader_priv.h`'s shared interpreter. Loading does
not execute LLVM, native machine code or downloaded code. D3D10's profile queries
remain `vs_4_0` / `ps_4_0`; acceptance of newer shader blobs is a WRLD extension,
not a D3D11/12 API implementation or feature-level claim.

- **DXBC:** arithmetic, conversions/comparisons and integer/bit operations;
  structured if/else/loop/break/continue, return/discard; constant buffers with
  relative addressing, immediate buffers and indexable temporaries. SM5 adds
  `rcp`, half conversions, count/reverse/first-bit operations, bitfield extraction /
  insertion, carry/borrow and `swapc`. Legacy SM4 texel loads/size queries remain.
- **DXIL:** scalar f32/i32 and booleans, supported arithmetic/casts/comparisons /
  selects, common unary/binary/tertiary math and dot products, signature I/O,
  legacy constant-buffer loads at constant rows, legacy resource handles,
  float Texture2D `Sample` / `SampleLevel`, half conversions and discard.
  Forward-only, topologically ordered acyclic basic blocks and two-input phi
  nodes are supported, with predecessor/dominance checks. No helper functions,
  pointers/memory instructions or vector LLVM arithmetic are executed.
- **Resources:** fourteen constant-buffer slots and sixteen texture/sampler slots
  in register space zero. Sparse bindings and constant indices into bounded
  resource ranges are supported; dynamic/nonuniform binding selection is not.
  Modern explicit LOD/bias must be literal zero. Texture arrays/cubes/MSAA,
  comparison samplers, nonzero offsets, modern texel loads and nonzero/dynamic
  LOD queries are rejected, not silently approximated. Only the base mip is
  stored/rendered by the existing D3D10 texture backend.
- **Pipeline:** packed signature components are mapped by semantic, including
  different VS/PS shader models. Perspective, noperspective and flat interpolation
  work at pixel centers (single-sample centroid is equivalent); sample-frequency
  or mixed-mode packing within one register is rejected. Uint color outputs use
  `R8G8B8A8_UINT` / `R32_UINT` targets without blending. Signed color targets,
  multiple render targets and conservative depth outputs are not implemented.
- **Explicitly unsupported:** geometry/tessellation/compute, SM5.1 / SM6.1+,
  waves, atomics/UAVs, doubles, screen derivatives, helper functions, backward
  DXIL branches, extended shader properties and unsupported intrinsics. The
  broader D3D10 API still lacks stencil, MSAA, line/point primitives, stream
  output and run-time HLSL compilation. This is not a complete DXIL validator or
  bit-for-bit GPU floating-point conformance implementation.

### Bounds and lifetime

Containers are at most 16 MiB; DXIL bitcode at most 1 MiB. Programs are limited to
65,536 native instructions, 32,767 operands, 4,096 ordinary and 4,096 indexable
temporary vectors; the decoder limits nesting to 16 operand / 64 structured
control levels. There are 32 signature entries and 31 ordinary I/O registers;
the final output slot is reserved for scalar depth. The first DXIL reader also
bounds types (512), SSA values (8,192), metadata nodes (4,096, 16 references per
node), basic blocks (128), record values (2,048) and bitstream block nesting (16).
Exceeding a bound rejects upload. An invocation has a one-million-instruction
budget and returns false on discard/budget exhaustion; allocation failure at
context preparation also fails without running the draw.

`dxbc_exec_init` binds register arrays and constant-buffer addresses/lengths once
per draw; `dxbc_exec_run` allocates nothing. Register/CB **contents** and primitive
IDs may change, but their bindings must stay stable until `dxbc_exec_fini`.
Contexts are caller-owned, not shared between concurrent draws. `dxbc_run` is an
allocating convenience wrapper for one-off callers, not the renderer hot path.

## Run tests

From the repository root on an x86/x64 host with GCC/Clang and libm:

```sh
make check-shaders
make bench-shaders
```

The first target runs the real loader/interpreter plus `d3d10.c`'s actual draw
path (only process/window plumbing is stubbed). It checks real compiler results,
prepared/convenience consistency, both branch outcomes, packed signatures,
sparse bindings, SM4/5/6 cube equivalence, integer/half/rounding behavior,
all 65,536 half encodings, allocation failure, malformed/truncated/mutated inputs,
instruction limits, zero hot-path allocations, coverage/shared-edge blending,
depth/discard, color write masks, perspective/flat/noperspective clipping and
unsigned render targets, native point/bilinear sampling and negative-coordinate
wrapping. Unsupported cases have dedicated rejection fixtures.
The core and framebuffer tests also run with Clang's undefined-behavior sanitizer.

The microbenchmark uses the original cube PS, one million invocations and the
same synthetic sampler/inputs as the saved pre-change baseline. Its checksum is
`218514.984`; **invocations per second are not OS frame rate**.
In seven interleaved x64 host trials (Zig 0.13.0 `cc`, GNU11, `-O2 -fno-builtin`,
Windows host CRT), median time fell from 0.070 to 0.053 seconds
(1.32x); both paths had zero hot-path allocations for this small shader. Larger
temporary banks are separately checked for zero allocations in prepared runs.

Inside WRLD (3D acceleration enabled):

```text
C:/Samples/shaderprobe.exe
C:/Samples/d3d10cube.exe --bench
C:/Samples/d3d10cube.exe --sm5 --bench
C:/Samples/d3d10cube.exe --sm6 --bench
```

Use forward slashes in Command Prompt: its shell consumes backslash escapes.
The cube's normal interactive mode still runs until Escape/close. The probe logs
16 successful-upload / rejection / unchanged-profile checks through
`OutputDebugString`. Bench mode uses four warm-up frames and 120 deterministic
480x360 frames, reporting guest milliseconds the same way.

### Measured x64 QEMU run

Same host, one vCPU, default QEMU TCG, 512 MiB, standard VGA, identical sample
binary/settings, disposable ISOs derived from `test-shape2.iso`:

Two runs per path (ranges, not guaranteed performance):

| Kernel / shader path | 120 frames (ms) | FPS |
| --- | ---: | ---: |
| Pre-change `78c52bc`, SM4 | 14,782–15,062 | 7.97–8.12 |
| Prepared/spans, SM4 | 9,108–9,882 | 12.14–13.18 |
| Prepared/spans, SM5 DXBC | 9,982–11,194 | 10.72–12.02 |
| Prepared/spans, SM6 DXIL | 13,746–15,385 | 7.80–8.73 |

The paired SM4 speedups are 1.52x and 1.62x (about 52–62% more FPS, 34–38% less
frame time). SM6's scalarized program has more interpreted work; newer profiles do not inherently
render faster. The x86 kernel path is compile-checked and both host suites can
run without SSE, but no new x86 OS FPS figure is claimed. Source ISO SHA256 was
unchanged: `4a683ff299b6cf7de82e8acea5c4fe616bd865f26cee78cafe8943b4923b1726`.

## Regenerate fixtures

Windows `d3dcompiler_47.dll!D3DCompile` emits the SM4/SM5 DXBC fixtures. DXC
v1.9.2609 emits the SM6 DXIL and readable `.ll` compiler disassemblies:

```powershell
python tools/build_shader_fixtures.py --dxc C:/path/to/dxc.exe
```

Or `make shader-fixtures DXC=C:/path/to/dxc.exe`. Without `--dxc`, only FXC outputs
are regenerated; normal host tests use all checked-in fixtures and need neither
compiler. The generator also refreshes the embedded cube/probe shader headers.
Rebuild `samples` afterward with MinGW-w64 targeting the classic MSVCRT supported
by WRLD, not a UCRT-only startup. The x86 samples also need an x87-compatible CRT
(the checked-in x86 binaries use w64devkit 2.10): source-level `-mno-sse` cannot
remove SSE2 from a newer prebuilt CRT. The Makefile supplies x87 flags for these
two x86 samples; a small sample-local `_matherr` hook also avoids pulling in the
CRT's optional SIMD diagnostic printer. Their executable sections are audited
for SSE/AVX/MMX instructions. `graphics.hlsl` owns general fixtures,
`rejected.hlsl` owns deliberately unsupported resources/profiles, and
`samples/d3d10cube.hlsl` mirrors the original `samples/d3d10cube.sm4` workload.
