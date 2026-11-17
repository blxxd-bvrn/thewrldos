// FXC/DXC equivalents of d3d10cube.sm4. tools/build_shader_fixtures.py emits
// the checked-in SM5 DXBC / SM6 DXIL used by --sm5 and --sm6; no OS compiler.
cbuffer Scene : register(b0) { float4 Data[10]; };
Texture2D<float4> Picture : register(t0);
SamplerState Filtering : register(s0);
struct Input { float3 position : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct Varying { float4 position : SV_Position; float2 uv : TEXCOORD0; float4 color : COLOR0; };
Varying vertex(Input input) {
    Varying output;
    float4 p = float4(input.position, 1);
    output.position = float4(dot(p, Data[0]), dot(p, Data[1]), dot(p, Data[2]), dot(p, Data[3]));
    float3 n = float3(dot(input.normal, Data[4].xyz), dot(input.normal, Data[5].xyz), dot(input.normal, Data[6].xyz));
    float diffuse = saturate(dot(normalize(n), Data[8].xyz));
    output.color = float4(.25 + diffuse * .8, .25 + diffuse * .8, .25 + diffuse * .8, 1);
    output.uv = input.uv;
    return output;
}
float4 pixel(Varying input) : SV_Target {
    float4 rgba = Picture.Sample(Filtering, input.uv) * input.color;
    float band = frac(input.uv.x + input.uv.y + Data[9].x);
    if (band < .12) rgba.rgb = rgba.rgb * .6 + .4;
    return rgba;
}
