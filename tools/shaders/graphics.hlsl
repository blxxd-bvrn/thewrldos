// Native shader-engine fixtures. FXC/D3DCompile makes SM4/5 DXBC; DXC makes
// genuine SM6 DXIL from the same source. Neither compiler is an OS dependency.
cbuffer Scene : register(b0) {
    float4 Tint;
    float4x4 Transform;
};
Texture2D<float4> Picture : register(t0);
SamplerState Filtering : register(s0);

struct Input { float4 position : POSITION; float4 color : COLOR0; float2 uv : TEXCOORD0; };
struct Varying { float4 position : SV_Position; float4 color : COLOR0; float2 uv : TEXCOORD0; };

Varying vertex(Input input) {
    Varying output;
    output.position = mul(Transform, input.position);
    output.color = input.color * Tint;
    output.uv = input.uv;
    return output;
}

float4 color(Varying input) : SV_Target {
    float band = frac(input.uv.x + input.uv.y + Tint.x);
    float4 value = input.color * Tint;
    return band < 0.25 ? saturate(value + 0.1) : value;
}

float4 texture_level(Varying input) : SV_Target {
    return Picture.SampleLevel(Filtering, input.uv, 0.0) * input.color;
}

float4 reciprocal(Varying input) : SV_Target {
    return rcp(input.color + 2.0);
}

uint4 bits(uint4 value : COLOR0) : SV_Target {
    return uint4(countbits(value.x), reversebits(value.y), firstbitlow(value.z), firstbithigh(value.w));
}

struct Packed {
    float4 position : SV_Position;
    float2 low : TEXCOORD0;
    float2 high : TEXCOORD1;
    float scalar : TEXCOORD2;
};
Packed packed_vertex(Input input) {
    Packed output;
    output.position = input.position;
    output.low = input.uv;
    output.high = input.color.xy;
    output.scalar = input.color.z;
    return output;
}
float4 packed_pixel(Packed input) : SV_Target {
    return float4(input.low + input.high, input.scalar, 1.0);
}

cbuffer SparseScene : register(b3) { float4 SparseTint; };
Texture2D<float4> SparsePicture : register(t7);
SamplerState SparseFiltering : register(s5);
float4 sparse(Varying input) : SV_Target {
    return SparsePicture.SampleLevel(SparseFiltering, input.uv, 0.0) * SparseTint;
}

float4 half_round(Varying input) : SV_Target {
    return f16tof32(f32tof16(input.color));
}
float4 maths(Varying input) : SV_Target {
    return float4(sin(input.color.x), cos(input.color.y), floor(input.color.z), round(input.color.w));
}
struct DepthResult { float4 color : SV_Target; float depth : SV_Depth; };
DepthResult depth(Varying input) {
    DepthResult output;
    output.color = input.color;
    output.depth = 0.25;
    return output;
}
float4 flat_color(nointerpolation float4 value : COLOR0) : SV_Target { return value; }
float4 linear_color(noperspective float4 value : COLOR0) : SV_Target { return value; }
float4 smooth_color(float4 value : COLOR0) : SV_Target { return value; }
float4 clip_color(Varying input) : SV_Target { clip(input.uv.x - .5); return input.color; }
struct UIntInput { float4 position : POSITION; uint4 color : COLOR0; };
struct UIntVarying { float4 position : SV_Position; nointerpolation uint4 color : COLOR0; };
UIntVarying uint_vertex(UIntInput input) {
    UIntVarying output; output.position = input.position; output.color = input.color; return output;
}
float4 sample_implicit(Varying input) : SV_Target { return Picture.Sample(Filtering, input.uv); }
float4 lod_one(Varying input) : SV_Target { return Picture.SampleLevel(Filtering, input.uv, 1.0); }

float4 derivative(Varying input) : SV_Target { return ddx(input.color); }
TextureCube<float4> CubePicture : register(t1);
float4 cube_texture(Varying input) : SV_Target { return CubePicture.SampleLevel(Filtering, input.color.xyz, 0.0); }

// Deliberately unsupported by the first native DXIL frontend.
float4 wave(Varying input) : SV_Target {
    return WaveActiveSum(input.color);
}

[numthreads(1, 1, 1)]
void compute(uint3 id : SV_DispatchThreadID) { }

float4 dynamic_loop(Varying input) : SV_Target {
    float4 value = input.color;
    [loop] for (int i = 0; i < (int)input.uv.x; i++) value = value * 0.75 + 0.01;
    return value;
}
