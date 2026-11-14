# Native shader tests and staged compatibility

These are **owned HLSL sources and genuine Microsoft-compiler outputs**, not a
private bytecode dialect. The engine stays native C; the compilers are host
fixture-generation tools, never OS runtime dependencies.

## Tested scope

| Frontend | Accepted stage/profile subset |
| --- | --- |
| DXBC | Vertex/pixel SM4.0/4.1 and SM5.0 token programs in DXBC containers |
| DXIL | Vertex/pixel SM6.0, DXIL 1.0, LLVM bitcode version 1; one void entry function |

`dxbc.c` and `dxil.c` lower to `shader_priv.h`'s shared native IR/interpreter. Eligible
D3D10 draws can also lower that IR to legacy VMware GPU shaders (see below). Loading does
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
wrapping. GPU tests check per-draw CPU retry, reference-device selection, D24 CPU/GPU
depth coherence, native IR-to-SM3 reference execution, stage opcodes, constant/input
read ports, aliased scalar/conditional operations, ordered comparisons (including
NaNs), and expanded instruction budgets. Mocked port I/O exercises the actual FIFO
code's atomic/wrapped packets, timeouts, fence wrap and 2D capability refresh;
obsolete "DEAD" slots/cursor IDs are not device-error status. Unsupported cases have
dedicated rejection fixtures. These are host tests, not proof that a GPU executed
the draw. The suites also run with Clang's undefined-behavior sanitizer.

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
C:/Samples/d3d10cube.exe --bench --cpu
C:/Samples/gpuverify.exe --show
```

Use forward slashes in Command Prompt: its shell consumes backslash escapes.
The cube's normal interactive mode still runs until Escape/close. The probe logs
16 successful-upload / rejection / unchanged-profile checks through
`OutputDebugString`. Bench mode uses four warm-up frames and 120 deterministic
480x360 frames, reporting guest milliseconds, the last completed draw's backend,
and per-device GPU/CPU draw counts the same way. `--cpu` uses a D3D10 reference
device to force native CPU rendering; combine it with each shader-model switch
for like-for-like comparisons. The optional `WRLDGetD3D10Backend` /
`WRLDGetD3D10DrawStats` exports are diagnostics, not Direct3D feature levels.

### Native legacy VMware GPU subset

The guest backend is native C (`src/kernel/gpu3d.c`, `src/win32/shader_gpu.c`),
without Mesa, LLVM, FreeType, Skia or ICU dependencies. It is tested with Workstation
15.0.0 build-10134415, virtual hardware **7**, `mks.enable3d = "TRUE"` and
`svga.vgpu10 = "FALSE"`. VMware's DX11 host renderer used an RTX 3050; the guest
does not contain an NVIDIA driver. Modern guest-backed-object SVGA3D (including
the tested hardware-16 configuration) is not implemented. Missing capabilities,
failed DMA/rendered-triangle probes, and unsupported draws keep native CPU rendering.

Hardware lowering is deliberately narrower than shader upload: up to 28 original
temporary vectors, 512 emitted SM3 instruction slots, 24 branch levels, ten
perspective varying registers, and eight 2D texture/sampler pairs. Float math and
tracked comparison booleans support bounded if/else; integer/bit operations,
relative/indexable operands, loops/discard, implicit texture gradients inside
dynamic flow, unsupported linkage and larger programs fall back per draw.
SM3 pixel shaders cannot use vertex-only SLT/SGE, and both stages allow only one
distinct input and one distinct constant register per instruction. Legalization
and alias-safe scalar comparisons are covered by the host reference tests.

RGBA8/BGRA8 targets, base-level point/linear sampling, wrap/mirror/clamp/border,
triangle lists/strips, and common depth/cull/blend/color-mask states offload;
unsupported states retain the CPU path. The legacy target uses D24S8 with depth in
the upper 24 bits; D32F requests fall back if float depth is unavailable. CPU
buffers remain authoritative, with a stable bounded 16 MiB staging GMR, uploads
before each draw and color/depth readbacks committed only after a fence. Staging
is deliberately retained after timeout to prevent late host DMA into freed memory.

`gpuverify.exe` requires actual completed GPU draws and compares CPU/GPU readbacks
at four deterministic poses for the original SM4 and real-compiler SM5 cubes:
36 checks, mean RGB error <= 2/255 and at most 2% of pixels with any RGB error > 24/255.
The tested run has zero such bad pixels, with mean errors 0.002-0.021/255. The
larger SM6 cube exceeds this initial hardware subset and correctly stays on the CPU.
Do not substitute a CPU-versus-CPU comparison or relax tolerances to hide failures.
Per-draw synchronous readback is expensive; small-cube results do not establish a
universal FPS improvement. Earlier QEMU CPU measurements below
are not comparable to hardware-virtualized VMware timings.

Optional **Windows-host** SM3 assembler validation (not just disassembly):

```powershell
New-Item -ItemType Directory -Force build64/sm3 | Out-Null
./build64/shadergputest.exe build64/sm3
python tools/check_gpu_shaders.py build64/sm3
```

This uses the host's `d3dcompiler_47.dll` only. No compiler DLL or new runtime goes
into the guest. Native GPU readback validation is still required separately.

### Measured legacy VMware x64 runs

Two final trials on the configuration above, 512 MiB, one vCPU, deterministic
480x360 cube, four warm-up frames plus 120 timed frames. Each CPU/GPU pair uses
the same shader model and sample binary; timings include uploads, fenced color/
depth readback and window presentation:

| Shader | Forced CPU (ms) | GPU (ms) | Paired frame-rate speedups |
| --- | ---: | ---: | ---: |
| SM4 | 1,158 / 1,148 | 901 / 795 | 1.28x / 1.44x |
| SM5 | 1,224 / 1,252 | 1,167 / 908 | 1.05x / 1.38x |

Each GPU case completes 124 GPU draws and zero CPU draws; the SM6 cube completes
124 CPU fallback draws. All eight fixed-pose comparisons pass with zero bad
pixels. Both CPU-only configurations (legacy VMware with 3D disabled, and modern
hardware 16 with 3D enabled) retain CPU draws and pass the 16-check shader probe.
These are limited paravirtual-GPU measurements, not a promise for other workloads,
VMware versions or physical GPUs. No native x86 OS frame-rate claim is made.

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
shader samples (including `gpuverify`); a small sample-local `_matherr` hook also avoids pulling in the
CRT's optional SIMD diagnostic printer. Their executable sections are audited
for SSE/AVX/MMX instructions. `graphics.hlsl` owns general fixtures,
`rejected.hlsl` owns deliberately unsupported resources/profiles, and
`samples/d3d10cube.hlsl` mirrors the original `samples/d3d10cube.sm4` workload.
